#pragma once

#include <string>

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// Endpoint Type Classification
//===----------------------------------------------------------------------===//

//! Endpoint type for connection behavior (TLS, features)
enum class EndpointType {
	OnPremises,  // Traditional SQL Server (self-signed certs OK)
	AzureSQL,    // Azure SQL Database (hostname verification required)
	Fabric,      // Microsoft Fabric Warehouse (limited features)
	Synapse      // Azure Synapse Analytics (similar to AzureSQL)
};

//===----------------------------------------------------------------------===//
// Endpoint Detection Functions
//===----------------------------------------------------------------------===//

//! Determine endpoint type from hostname
//! @param host Server hostname
//! @return EndpointType classification
EndpointType GetEndpointType(const std::string &host);

//! Check if endpoint requires hostname verification (Azure endpoints)
//! @param type Endpoint type
//! @return true if TLS hostname verification should be performed
bool RequiresHostnameVerification(EndpointType type);

//! Check if endpoint is Azure-based (any cloud endpoint)
//! Matches: *.database.windows.net, *.fabric.microsoft.com, *.sql.azuresynapse.net
//! @param host Server hostname
//! @return true if this is an Azure endpoint requiring Azure AD auth support
bool IsAzureEndpoint(const std::string &host);

//! Check if endpoint is Microsoft Fabric
//! Matches: *.datawarehouse.fabric.microsoft.com, *.pbidedicated.windows.net
//! @param host Server hostname
//! @return true if this is a Fabric Warehouse endpoint (with limited features)
bool IsFabricEndpoint(const std::string &host);

//! Check if endpoint is Azure Synapse Analytics
//! Matches: *-ondemand.sql.azuresynapse.net
//! @param host Server hostname
//! @return true if this is a Synapse endpoint
bool IsSynapseEndpoint(const std::string &host);

}  // namespace mssql
}  // namespace duckdb
