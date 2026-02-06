# FEDAUTH Authentication Research for Microsoft Fabric

## Problem Statement

The mssql-extension fails to authenticate with Microsoft Fabric Data Warehouse using Azure AD (FEDAUTH) authentication. The connection authenticates to the Fabric gateway but fails when sending the FEDAUTH_TOKEN packet, with the server closing the connection (SSL_ERROR_SYSCALL).

## Reference Implementation: go-mssqldb

### Successful Flow (Method 1 - azuread driver, no routing)

```
1. PRELOGIN with FEDAUTHREQUIRED option
2. TLS handshake (wrapped in TDS PRELOGIN packets)
3. LOGIN7 with FEDAUTH feature extension (ADAL workflow)
4. Server responds with FEDAUTHINFO token (0xEE)
5. Client sends FEDAUTH_TOKEN packet (type 0x08)
6. Server responds with LOGINACK (0xAD)
7. Success
```

### Flow with Routing (Azure SQL / Fabric gateway)

```
1. PRELOGIN with FEDAUTHREQUIRED option
2. TLS handshake (wrapped in TDS PRELOGIN packets)
3. LOGIN7 with FEDAUTH feature extension (ADAL workflow)
4. Server responds with FEDAUTHINFO token (0xEE)
5. Client sends FEDAUTH_TOKEN packet (type 0x08)
6. Server responds with:
   - LOGINACK (0xAD) - partial success
   - ENVCHANGE type 20 (0x14) - ROUTING with new server:port
   - DONE token
7. Client closes connection to gateway
8. Client connects to routed server (e.g., pbidedicated.windows.net)
9. GOTO step 1 with new server (up to 5 routing hops allowed)
```

### go-mssqldb Routing Implementation

```go
// tds.go:1392 - After login loop completes
if sess.routedServer != "" {
    fmt.Printf("[ROUTING] Redirecting to: %s:%d\n", sess.routedServer, sess.routedPort)
    toconn.Close()

    // Parse routed server (may contain instance name: "host\instance")
    routedParts := strings.SplitN(sess.routedServer, "\\", 2)
    p.Host = routedParts[0]
    if len(routedParts) == 2 {
        p.Instance = routedParts[1]
    }
    p.Port = uint64(sess.routedPort)

    // Update TLS config for new host
    if !p.HostInCertificateProvided && p.TLSConfig != nil {
        p.TLSConfig = p.TLSConfig.Clone()
        p.TLSConfig.ServerName = p.Host
    }

    goto initiate_connection  // Restart entire flow
}
```

Key points:
- Routing is checked AFTER login loop completes (after LOGINACK)
- Routed server may include instance name (e.g., `host\instance-name`)
- TLS SNI must be updated for the new host
- The ENTIRE authentication flow restarts on the routed server

### When Routing Happens

From our testing:

| Scenario | Routing? | Notes |
|----------|----------|-------|
| go-mssqldb Method 1 (azuread driver) | NO | Direct connection, FEDAUTH works |
| go-mssqldb Method 2 (sqlserver driver) | YES | Routes to pbidedicated, then FAILS |
| Our implementation | YES (sometimes) | Server sends routing after FEDAUTHINFO |

Observation: When FEDAUTH extension is properly recognized by gateway, it may NOT route.
When FEDAUTH is not recognized (or wrong flow), gateway routes to dedicated server.

### LOGIN7 Feature Extension (FEDAUTH)

go-mssqldb builds an 8-byte FEDAUTH extension:

```
02 02 00 00 00 05 01 ff
│  │           │  │  │
│  │           │  │  └─ TERMINATOR (0xFF)
│  │           │  └──── ADAL_WORKFLOW_PASSWORD (0x01)
│  │           └─────── Options: (ADAL_LIBRARY << 1) | echo_bit = 0x05
│  └───────────────────── FeatureDataLen: 2 (little-endian)
└────────────────────── FeatureId: FEDAUTH (0x02)
```

Options byte calculation:
- FEDAUTH_LIBRARY_ADAL = 0x02
- echo_bit = 1 (if server's FEDAUTHREQUIRED was non-zero)
- Options = (0x02 << 1) | 1 = 0x05

### FEDAUTH_TOKEN Packet Format

```go
// go-mssqldb sendFedAuthInfo()
fedauthtoken := str2ucs2(fedAuth.FedAuthToken)  // UTF-16LE encode
tokenlen := len(fedauthtoken)
datalen := 4 + tokenlen + len(fedAuth.Nonce)

w.BeginPacket(packFedAuthToken, false)  // Type 0x08, status 0x00
binary.Write(w, binary.LittleEndian, uint32(datalen))
binary.Write(w, binary.LittleEndian, uint32(tokenlen))
w.Write(fedauthtoken)
w.Write(fedAuth.Nonce)  // Empty for ADAL
return w.FinishPacket()  // Sets status |= 0x01 (EOM)
```

go-mssqldb output:
```
DataLen: 3340 (0x00000D0C)
TokenLen: 3336 (0x00000D08)
Packet header bytes: 0C 0D 00 00 08 0D 00 00
```

### Response Processing Loop

go-mssqldb uses a token processing loop that continues after sending FEDAUTH_TOKEN:

```go
for loginAck := false; !loginAck; {
    for each token in response:
        case fedAuthInfoStruct:  // Token 0xEE
            // Get token from Azure AD provider
            fedAuth.FedAuthToken = adalTokenProvider(SPN, STS_URL)
            // Send FEDAUTH_TOKEN packet
            sendFedAuthInfo(outbuf, fedAuth)
            // CONTINUE LOOP - don't return yet

        case loginAckStruct:  // Token 0xAD
            loginAck = true  // EXIT loop

        case doneStruct:
            if error: return error
}
```

Key insight: The loop continues AFTER sending FEDAUTH_TOKEN to wait for LOGINACK.

### Routing Handling

If routing is detected (after LOGINACK), go-mssqldb reconnects to the routed server:

```go
if sess.routedServer != "" {
    toconn.Close()
    p.Host = routedParts[0]
    p.Port = routedPort
    goto initiate_connection  // Restart the whole flow
}
```

## Our Implementation

### Current Packet Output

```
TDS header: 08 01 0c d8 00 00 01 00
- Type: 0x08 (FEDAUTH_TOKEN) ✓
- Status: 0x01 (END_OF_MESSAGE) ✓
- Length: 0x0CD8 = 3288 bytes ✓
- SPID: 0x0000
- PacketId: 1 ✓
- Window: 0

Payload first 20 bytes: cc 0c 00 00 c8 0c 00 00 65 00 79 00 4a 00 30 00 65 00 58 00
- DataLen: 0x00000CCC = 3276 ✓ (= 4 + 3272)
- TokenLen: 0x00000CC8 = 3272 ✓
- Token starts: "eyJ0eX..." (UTF-16LE JWT) ✓
```

### Feature Extension

Our LOGIN7 last 20 bytes:
```
76 00 2d 00 77 00 68 00 76 01 00 00 02 02 00 00 00 05 01 ff
                       ^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^
                       ext_offset FEDAUTH extension
```

FEDAUTH extension: `02 02 00 00 00 05 01 ff` - **Matches go-mssqldb exactly**

### Issue (RESOLVED)

The server was closing the connection after receiving FEDAUTH_TOKEN. The root cause was identified in the PRELOGIN packet.

## Root Cause: PRELOGIN Differences

The Fabric gateway uses PRELOGIN fields to determine the client type and decides whether to:
1. Route the client to a dedicated backend server (ROUTING), OR
2. Start FEDAUTH flow directly on the gateway (FEDAUTHINFO)

**go-mssqldb gets ROUTING on first LOGIN7 from gateway** (correct flow):
1. Gateway → LOGINACK + ROUTING ENVCHANGE
2. Client connects to routed server (pbidedicated.windows.net)
3. Routed server → FEDAUTHINFO → FEDAUTH_TOKEN → SUCCESS

**Our old implementation got FEDAUTHINFO from gateway** (incorrect flow):
1. Gateway → FEDAUTHINFO (not ROUTING!)
2. FEDAUTH_TOKEN → Server closes connection

### PRELOGIN Differences (Root Cause)

| Field | go-mssqldb | Our (old) | Our (fixed) |
|-------|------------|-----------|-------------|
| VERSION format | LE driver version: `06 00 09 01 00 00` | BE SQL version: `0F 00 00 00 00 00` | LE driver version ✓ |
| INSTOPT (2) | `00` | Missing | `00` ✓ |
| THREADID (3) | `00 00 00 00` | Missing | `00 00 00 00` ✓ |
| MARS (4) | `00` | Missing | `00` ✓ |
| TRACEID (5) | 36 bytes (connid + activityid) | **Missing** | 36 bytes ✓ |
| FEDAUTHREQUIRED (6) | `01` | `01` ✓ | `01` ✓ |

**The key fix was adding TRACEID (field 5)** with 36 bytes of connection/activity UUIDs. Without this, the Fabric gateway didn't recognize us as a proper FEDAUTH-capable client.

## Comparison Table

| Aspect | go-mssqldb | Our Implementation |
|--------|------------|-------------------|
| PRELOGIN VERSION | LE driver version | LE driver version ✓ |
| PRELOGIN TRACEID | 36 bytes | 36 bytes ✓ |
| FEDAUTH extension | `02 02 00 00 00 05 01 ff` | `02 02 00 00 00 05 01 ff` ✓ |
| Packet type | 0x08 | 0x08 ✓ |
| Packet status | 0x01 (EOM) | 0x01 (EOM) ✓ |
| Packet ID | 1 | 1 ✓ |
| DataLen format | 4 bytes LE | 4 bytes LE ✓ |
| TokenLen format | 4 bytes LE | 4 bytes LE ✓ |
| Token encoding | UTF-16LE | UTF-16LE ✓ |
| FEDAUTHINFO parsing | ✓ | ✓ |

## Fix Summary

The PRELOGIN packet was updated to match go-mssqldb exactly:
1. **VERSION**: Changed from big-endian SQL Server version (15.0) to little-endian driver version (1.9.0.6)
2. **TRACEID**: Added 36-byte trace ID (16-byte connection UUID + 16-byte activity UUID + 4-byte sequence)

## Files Reference

### go-mssqldb (microsoft/go-mssqldb)
- `tds.go:1135` - connect() main flow
- `tds.go:742` - sendFedAuthInfo() - FEDAUTH_TOKEN packet
- `tds.go:1296` - prepareLogin() and sendLogin()
- `tds.go:1346` - fedAuthInfoStruct case handling
- `token.go:449` - parseFedAuthInfo()
- `buf.go:136` - BeginPacket()
- `buf.go:151` - FinishPacket()

### Our Implementation
- `src/tds/tds_connection.cpp:449` - DoLogin7WithFedAuth()
- `src/tds/tds_protocol.cpp:1348` - BuildLogin7WithADAL()
- `src/tds/tds_protocol.cpp:1564` - BuildFedAuthToken()
- `src/azure/azure_fedauth.cpp` - Token encoding

## Implementation Tasks

### Phase 1: Add Routing Support for SQL Auth (Non-FEDAUTH)

Currently, routing is only implemented in `AuthenticateWithFedAuth`. The regular `Authenticate()` function does not handle routing, which would fail for Azure SQL connections that require routing.

**Required changes:**
1. Refactor `DoLogin7()` to return routing info
2. Add routing loop to `Authenticate()` similar to `AuthenticateWithFedAuth()`
3. Handle reconnection to routed server

### Phase 2: Debug FEDAUTH_TOKEN Issue

1. Add wire-level packet capture to compare exact bytes sent
2. Compare JWT token contents between go-mssqldb and our implementation
3. Investigate if Azure token acquisition uses same scope/audience
4. Consider implementing continuous response loop like go-mssqldb
5. Test with TLS record size configuration

### Phase 3: Verify Full Flow

1. Test with Azure SQL Database (non-Fabric)
2. Test with Microsoft Fabric Data Warehouse
3. Test routing scenarios with both SQL auth and FEDAUTH
