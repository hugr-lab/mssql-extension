//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// krb5_authenticator.cpp
//
// POSIX GSSAPI implementation of IAuthenticator. SPNEGO + Kerberos against
// SQL Server's SSPI / Negotiate package.
//
// Spec 042. Compiled only when MSSQL_ENABLE_KRB5 is defined.
//===----------------------------------------------------------------------===//

#include "tds/auth/krb5_authenticator.hpp"

#if defined(MSSQL_ENABLE_KRB5)

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

// macOS's bundled GSS framework is a Heimdal-based subset that does NOT
// expose gss_acquire_cred_from / gss_key_value_set_desc or the full krb5.h
// (krb5_get_init_creds_password is not in the public macOS SDK). On macOS
// we therefore only support credential-cache mode (kinit/klist), which is
// the default and overwhelmingly the most common case. Keytab and raw
// modes require MIT Kerberos (Linux libgssapi-krb5 + libkrb5-dev).
//
// In containerized CI we always run Linux, so the test infrastructure
// exercises all three credential modes. macOS users who need keytab/raw
// modes can build the extension inside a Linux container.
#if !defined(__APPLE__)
#define MSSQL_KRB5_HAS_MIT_EXTENSIONS 1
#include <krb5.h>
#endif

namespace duckdb {
namespace tds {

namespace {

// SPNEGO mechanism OID: 1.3.6.1.5.5.2. DER form: 06 06 2b 06 01 05 05 02
// We construct a gss_OID_desc pointing into a static buffer because gss_OID
// is a pointer-to-struct and the buffer must outlive the call.
//
// SQL Server's SSPI implementation expects SPNEGO, NOT raw Kerberos. Sending
// raw Kerberos blobs (1.2.840.113554.1.2.2) makes the server drop the
// connection. See spec 042 research.md R4.
static const gss_OID_desc kSpnegoOidDesc = {6, const_cast<char *>("\x2b\x06\x01\x05\x05\x02")};
static const gss_OID kSpnegoOid = const_cast<gss_OID>(&kSpnegoOidDesc);

// Kerberos mechanism OID: 1.2.840.113554.1.2.2. Used only when issuing
// gss_acquire_cred for credential modes -- SPNEGO is a wrapper and the
// actual underlying mech inside the cred should be Kerberos so the AS-REQ
// goes to the KDC. Some GSSAPI implementations infer this automatically;
// passing it explicitly is safer.
static const gss_OID_desc kKrb5OidDesc = {9, const_cast<char *>("\x2a\x86\x48\x86\xf7\x12\x01\x02\x02")};
static const gss_OID kKrb5Oid = const_cast<gss_OID>(&kKrb5OidDesc);

// Hostbased-service name type OID: 1.2.840.113554.1.2.1.4 (RFC 2744 4.5).
// macOS's GSS framework declares GSS_C_NT_HOSTBASED_SERVICE as an extern
// gss_OID but does not export the symbol from the framework binary, so we
// construct the OID locally to keep the build portable.
static const gss_OID_desc kHostBasedServiceOidDesc = {10, const_cast<char *>("\x2a\x86\x48\x86\xf7\x12\x01\x02\x01\x04")};
static const gss_OID kHostBasedServiceOid = const_cast<gss_OID>(&kHostBasedServiceOidDesc);

// Kerberos principal name type OID: 1.2.840.113554.1.2.2.1 (RFC 1964 2.1.1).
// Same problem as above on macOS: GSS_KRB5_NT_PRINCIPAL_NAME is declared in
// the header but not exported.
static const gss_OID_desc kKrb5PrincipalNameOidDesc = {10,
														const_cast<char *>("\x2a\x86\x48\x86\xf7\x12\x01\x02\x02\x01")};
static const gss_OID kKrb5PrincipalNameOid = const_cast<gss_OID>(&kKrb5PrincipalNameOidDesc);

// Append a gss_display_status message chain (for one of major / minor) into out.
static void AppendGssStatusMessages(uint32_t code, int code_type, gss_OID mech, std::string &out) {
	OM_uint32 msg_ctx = 0;
	gss_buffer_desc msg_buf;
	int safety = 0;  // bound the loop so a misbehaving mech can't spin
	do {
		std::memset(&msg_buf, 0, sizeof(msg_buf));
		OM_uint32 minor_ignored = 0;
		OM_uint32 maj = gss_display_status(&minor_ignored, code, code_type, mech, &msg_ctx, &msg_buf);
		if (maj != GSS_S_COMPLETE || msg_buf.length == 0 || msg_buf.value == nullptr) {
			if (msg_buf.value && msg_buf.length > 0) {
				OM_uint32 dummy = 0;
				gss_release_buffer(&dummy, &msg_buf);
			}
			break;
		}
		if (!out.empty()) {
			out.append(": ");
		}
		out.append(static_cast<const char *>(msg_buf.value), msg_buf.length);
		OM_uint32 dummy = 0;
		gss_release_buffer(&dummy, &msg_buf);
		if (++safety > 8) {
			break;
		}
	} while (msg_ctx != 0);
}

// Look up file readability; used at construction-time for keytabs and
// configfiles so the error surfaces at ATTACH instead of at first call.
static bool FileIsReadable(const std::string &path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream f(path, std::ios::binary);
	return f.good();
}

// Spec 042 R8: map common cause hints into a hint string that is appended
// to the GSSAPI status text. Keeps the verbatim status visible (for web
// search) while giving the user an actionable next step.
//
// On macOS the krb5 error code constants aren't available (Heimdal subset),
// so we fall back to substring matching on the gss_display_status text in
// the caller. Linux gets the precise constant-based mapping.
static const char *HintForMinor(uint32_t minor) {
#if defined(MSSQL_KRB5_HAS_MIT_EXTENSIONS)
	switch (static_cast<krb5_error_code>(minor)) {
	case KRB5_FCC_NOFILE:
	case KRB5_CC_NOTFOUND:
		return "(Hint: no credentials cache. Run 'kinit <user>@<REALM>' first.)";
	case KRB5KRB_AP_ERR_TKT_EXPIRED:
	case KRB5KRB_AP_ERR_TKT_NYV:
		return "(Hint: Kerberos ticket expired. Run 'kinit' to refresh.)";
	case KRB5KDC_ERR_S_PRINCIPAL_UNKNOWN:
		return "(Hint: server SPN not registered. Verify with 'setspn -L <account>' on Windows admin host.)";
	case KRB5KRB_AP_ERR_SKEW:
		return "(Hint: clock skew between client and KDC exceeds 5 minutes. Sync system clock via ntp/chrony.)";
	case KRB5_KDC_UNREACH:
	case KRB5_REALM_CANT_RESOLVE:
		return "(Hint: KDC unreachable. Check /etc/krb5.conf and network path to the AD domain controllers.)";
	case KRB5KRB_AP_ERR_BAD_INTEGRITY:
		return "(Hint: preauthentication failed -- wrong password.)";
	case KRB5_KT_NOTFOUND:
		return "(Hint: no matching key in keytab for the supplied principal.)";
	default:
		return nullptr;
	}
#else
	(void)minor;
	return nullptr;
#endif
}

}  // namespace

Krb5Authenticator::Krb5Authenticator(Krb5Config config) : config_(std::move(config)) {
	// Determine the credential acquisition mode based on which fields the
	// connection-string / secret parser populated. Mode is decided up-front
	// so misconfigurations surface here, not deep inside InitialBytes.
	if (!config_.keytabfile.empty()) {
#if defined(MSSQL_KRB5_HAS_MIT_EXTENSIONS)
		mode_ = Krb5CredentialMode::Keytab;
		if (!FileIsReadable(config_.keytabfile)) {
			throw std::runtime_error("MSSQL Kerberos auth: keytab file '" + config_.keytabfile +
									 "' is not readable. Check the path and file permissions.");
		}
#else
		throw std::runtime_error(
			"MSSQL Kerberos auth: keytab mode (krb5-keytabfile) is not supported on this platform. "
			"macOS's bundled GSS framework lacks the required MIT extensions. "
			"Build the extension on Linux with libkrb5-dev, or use credential-cache mode after kinit.");
#endif
	} else if (!config_.raw_username.empty() && !config_.raw_password.empty()) {
#if defined(MSSQL_KRB5_HAS_MIT_EXTENSIONS)
		mode_ = Krb5CredentialMode::Raw;
		if (config_.realm.empty() && config_.raw_username.find('@') == std::string::npos) {
			throw std::runtime_error(
				"MSSQL Kerberos auth: raw-credentials mode requires either krb5-realm or a "
				"fully-qualified User Id (user@REALM).");
		}
#else
		throw std::runtime_error(
			"MSSQL Kerberos auth: raw-credentials mode is not supported on this platform. "
			"macOS's bundled GSS framework lacks krb5_get_init_creds_password. "
			"Use credential-cache mode (kinit) instead, or build on Linux with libkrb5-dev.");
#endif
	} else {
		mode_ = Krb5CredentialMode::CredCache;
	}

	if (config_.spn.empty()) {
		throw std::runtime_error(
			"MSSQL Kerberos auth: empty SPN. AuthStrategyFactory should derive 'MSSQLSvc@<fqdn>' "
			"from the host before constructing Krb5Authenticator.");
	}

	// Apply per-connection krb5.conf override via env. Must happen before
	// any GSSAPI / krb5 call. Threaded clients running multiple parallel
	// ATTACHes with different krb5.conf overrides are out of scope -- this
	// is a process-global setting.
	if (!config_.configfile.empty()) {
		if (FileIsReadable(config_.configfile)) {
			setenv("KRB5_CONFIG", config_.configfile.c_str(), /*overwrite=*/1);
		} else {
			throw std::runtime_error("MSSQL Kerberos auth: krb5.conf '" + config_.configfile +
									 "' is not readable.");
		}
	}
	if (!config_.credcachefile.empty()) {
		setenv("KRB5CCNAME", config_.credcachefile.c_str(), /*overwrite=*/1);
	}
}

Krb5Authenticator::~Krb5Authenticator() {
	Free();
}

void Krb5Authenticator::ThrowGssError(const char *what, uint32_t major, uint32_t minor, gss_OID mech) {
	std::string text(what);
	text.append(": gss_major=");
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(major));
		text.append(buf);
	}
	text.append(", ");

	std::string major_text;
	AppendGssStatusMessages(major, GSS_C_GSS_CODE, GSS_C_NO_OID, major_text);
	if (!major_text.empty()) {
		text.append(major_text);
	} else {
		text.append("(no major status text)");
	}

	std::string minor_text;
	AppendGssStatusMessages(minor, GSS_C_MECH_CODE, mech ? mech : kKrb5Oid, minor_text);
	if (!minor_text.empty()) {
		text.append(" / ");
		text.append(minor_text);
	}

	const char *hint = HintForMinor(minor);
	if (hint) {
		text.append(" ");
		text.append(hint);
	}

	throw std::runtime_error(std::string("MSSQL Kerberos auth failed: ") + text);
}

void Krb5Authenticator::AcquireCredentials() {
	if (acquired_) {
		return;
	}

	OM_uint32 major = 0;
	OM_uint32 minor = 0;

	switch (mode_) {
	case Krb5CredentialMode::CredCache:
		// Use the default credentials -- GSS_C_NO_CREDENTIAL on gss_init_sec_context
		// later. No acquisition needed here; the kernel ccache lookup happens
		// inside the mech.
		cred_ = GSS_C_NO_CREDENTIAL;
		break;

#if defined(MSSQL_KRB5_HAS_MIT_EXTENSIONS)
	case Krb5CredentialMode::Keytab: {
		// Build a key/value pair telling GSSAPI to use a specific keytab.
		// gss_acquire_cred_from is available in MIT Kerberos >= 1.11.
		gss_key_value_element_desc elements[1];
		elements[0].key = "client_keytab";
		elements[0].value = config_.keytabfile.c_str();
		gss_key_value_set_desc cred_store;
		cred_store.count = 1;
		cred_store.elements = elements;

		// Import the desired client principal (if supplied), else use GSS_C_NO_NAME
		// and let the keytab's default entry win.
		gss_name_t desired_name = GSS_C_NO_NAME;
		gss_buffer_desc name_buf;
		std::string principal_str;
		if (!config_.raw_username.empty()) {
			principal_str = config_.raw_username;
			if (principal_str.find('@') == std::string::npos && !config_.realm.empty()) {
				principal_str.append("@").append(config_.realm);
			}
			name_buf.value = const_cast<char *>(principal_str.c_str());
			name_buf.length = principal_str.size();
			major = gss_import_name(&minor, &name_buf, kKrb5PrincipalNameOid, &desired_name);
			if (GSS_ERROR(major)) {
				ThrowGssError("gss_import_name (keytab principal)", major, minor);
			}
		}

		gss_OID_set_desc mech_set;
		mech_set.count = 1;
		mech_set.elements = const_cast<gss_OID>(kKrb5Oid);

		major = gss_acquire_cred_from(&minor, desired_name, GSS_C_INDEFINITE, &mech_set, GSS_C_INITIATE,
									  &cred_store, &cred_, nullptr, nullptr);
		if (desired_name != GSS_C_NO_NAME) {
			OM_uint32 d = 0;
			gss_release_name(&d, &desired_name);
		}
		if (GSS_ERROR(major)) {
			ThrowGssError("gss_acquire_cred_from (keytab)", major, minor);
		}
		break;
	}

	case Krb5CredentialMode::Raw: {
		// Run the krb5_get_init_creds_password flow into a MEMORY ccache, then
		// hand the ccache to GSSAPI via gss_acquire_cred_from with ccache=MEMORY:.
		// This is the same approach go-mssqldb takes for raw-credentials mode.
		krb5_context kctx = nullptr;
		krb5_error_code kerr = krb5_init_context(&kctx);
		if (kerr) {
			throw std::runtime_error("MSSQL Kerberos auth failed: krb5_init_context failed (code " +
									 std::to_string(static_cast<int>(kerr)) + ")");
		}

		std::string principal_str = config_.raw_username;
		if (principal_str.find('@') == std::string::npos && !config_.realm.empty()) {
			principal_str.append("@").append(config_.realm);
		}

		krb5_principal client = nullptr;
		kerr = krb5_parse_name(kctx, principal_str.c_str(), &client);
		if (kerr) {
			krb5_free_context(kctx);
			throw std::runtime_error("MSSQL Kerberos auth failed: krb5_parse_name failed for '" + principal_str +
									 "' (code " + std::to_string(static_cast<int>(kerr)) + ")");
		}

		krb5_creds creds;
		std::memset(&creds, 0, sizeof(creds));
		kerr = krb5_get_init_creds_password(kctx, &creds, client, config_.raw_password.c_str(), nullptr, nullptr,
											0, nullptr, nullptr);
		if (kerr) {
			krb5_free_principal(kctx, client);
			const char *kmsg = krb5_get_error_message(kctx, kerr);
			std::string err = "krb5_get_init_creds_password failed: " + std::string(kmsg ? kmsg : "(no detail)");
			if (kmsg) {
				krb5_free_error_message(kctx, kmsg);
			}
			krb5_free_context(kctx);
			throw std::runtime_error("MSSQL Kerberos auth failed: " + err);
		}

		// Store creds in a MEMORY ccache keyed on the principal so multiple
		// concurrent raw-mode connections don't trample each other.
		std::string ccname = std::string("MEMORY:mssql_raw_") + principal_str;
		krb5_ccache cc = nullptr;
		kerr = krb5_cc_resolve(kctx, ccname.c_str(), &cc);
		if (kerr) {
			krb5_free_cred_contents(kctx, &creds);
			krb5_free_principal(kctx, client);
			krb5_free_context(kctx);
			throw std::runtime_error("MSSQL Kerberos auth failed: krb5_cc_resolve failed");
		}
		krb5_cc_initialize(kctx, cc, client);
		krb5_cc_store_cred(kctx, cc, &creds);
		krb5_cc_close(kctx, cc);
		krb5_free_cred_contents(kctx, &creds);
		krb5_free_principal(kctx, client);
		krb5_free_context(kctx);

		gss_key_value_element_desc elements[1];
		elements[0].key = "ccache";
		elements[0].value = ccname.c_str();
		gss_key_value_set_desc cred_store;
		cred_store.count = 1;
		cred_store.elements = elements;

		gss_OID_set_desc mech_set;
		mech_set.count = 1;
		mech_set.elements = const_cast<gss_OID>(kKrb5Oid);

		major = gss_acquire_cred_from(&minor, GSS_C_NO_NAME, GSS_C_INDEFINITE, &mech_set, GSS_C_INITIATE,
									  &cred_store, &cred_, nullptr, nullptr);
		if (GSS_ERROR(major)) {
			ThrowGssError("gss_acquire_cred_from (raw ccache)", major, minor);
		}
		break;
	}
#endif	// MSSQL_KRB5_HAS_MIT_EXTENSIONS
	}

	// Import target SPN. We use GSS_C_NT_HOSTBASED_SERVICE which expects
	// the "@" separator form ("MSSQLSvc@host.example.com"), NOT the slash
	// form. Using the wrong name type or separator silently produces a
	// ticket the server cannot decrypt -- this is a well-documented FreeTDS
	// pitfall (see spec 042 research.md R2).
	gss_buffer_desc spn_buf;
	spn_buf.value = const_cast<char *>(config_.spn.c_str());
	spn_buf.length = config_.spn.size();
	major = gss_import_name(&minor, &spn_buf, kHostBasedServiceOid, &target_name_);
	if (GSS_ERROR(major)) {
		ThrowGssError("gss_import_name (SPN)", major, minor);
	}

	acquired_ = true;
}

std::vector<uint8_t> Krb5Authenticator::DoSecContextStep(const uint8_t *input_blob, size_t input_blob_len) {
	gss_buffer_desc input_token = GSS_C_EMPTY_BUFFER;
	if (input_blob && input_blob_len > 0) {
		input_token.value = const_cast<uint8_t *>(input_blob);
		input_token.length = input_blob_len;
	}
	gss_buffer_desc output_token = GSS_C_EMPTY_BUFFER;
	OM_uint32 major = 0;
	OM_uint32 minor = 0;
	OM_uint32 ret_flags = 0;
	OM_uint32 time_rec = 0;
	gss_OID actual_mech = GSS_C_NO_OID;

	major = gss_init_sec_context(&minor, cred_, &ctx_, target_name_, kSpnegoOid,
								 GSS_C_MUTUAL_FLAG | GSS_C_REPLAY_FLAG | GSS_C_SEQUENCE_FLAG | GSS_C_INTEG_FLAG,
								 GSS_C_INDEFINITE, GSS_C_NO_CHANNEL_BINDINGS,
								 (input_token.length > 0 ? &input_token : GSS_C_NO_BUFFER), &actual_mech, &output_token,
								 &ret_flags, &time_rec);

	if (GSS_ERROR(major)) {
		if (output_token.value) {
			OM_uint32 d = 0;
			gss_release_buffer(&d, &output_token);
		}
		ThrowGssError("gss_init_sec_context", major, minor, actual_mech);
	}

	std::vector<uint8_t> out;
	if (output_token.value && output_token.length > 0) {
		const uint8_t *p = static_cast<const uint8_t *>(output_token.value);
		out.assign(p, p + output_token.length);
	}
	if (output_token.value) {
		OM_uint32 d = 0;
		gss_release_buffer(&d, &output_token);
	}

	if (major == GSS_S_COMPLETE) {
		complete_ = true;
	}
	return out;
}

std::vector<uint8_t> Krb5Authenticator::InitialBytes() {
	AcquireCredentials();
	auto blob = DoSecContextStep(nullptr, 0);
	if (blob.empty() && !complete_) {
		throw std::runtime_error(
			"MSSQL Kerberos auth failed: GSSAPI returned an empty initial SPNEGO token. "
			"Check that SPNEGO is supported by the underlying mech (krb5).");
	}
	return blob;
}

std::vector<uint8_t> Krb5Authenticator::NextBytes(const std::vector<uint8_t> &server_blob) {
	if (complete_) {
		// Server sent more data after we were done. Some KDC/server configs
		// emit a final "OK" wrapper -- just return an empty blob.
		return {};
	}
	return DoSecContextStep(server_blob.empty() ? nullptr : server_blob.data(), server_blob.size());
}

void Krb5Authenticator::Free() {
	if (ctx_ != GSS_C_NO_CONTEXT) {
		OM_uint32 d = 0;
		gss_delete_sec_context(&d, &ctx_, GSS_C_NO_BUFFER);
		ctx_ = GSS_C_NO_CONTEXT;
	}
	if (cred_ != GSS_C_NO_CREDENTIAL) {
		OM_uint32 d = 0;
		gss_release_cred(&d, &cred_);
		cred_ = GSS_C_NO_CREDENTIAL;
	}
	if (target_name_ != GSS_C_NO_NAME) {
		OM_uint32 d = 0;
		gss_release_name(&d, &target_name_);
		target_name_ = GSS_C_NO_NAME;
	}
	complete_ = true;
	acquired_ = false;
}

}  // namespace tds
}  // namespace duckdb

#endif	// MSSQL_ENABLE_KRB5
