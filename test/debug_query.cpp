#include <iostream>
#include <memory>
#include "tds/tds_connection.hpp"
#include "tds/tds_protocol.hpp"
#include "tds/tds_packet.hpp"

using namespace duckdb::tds;

int main() {
    std::cout << "Creating connection..." << std::endl;

    auto conn = std::make_shared<TdsConnection>();

    std::cout << "Connecting to localhost:1433..." << std::endl;
    if (!conn->Connect("localhost", 1433)) {
        std::cerr << "Connect failed: " << conn->GetLastError() << std::endl;
        return 1;
    }
    std::cout << "Connected!" << std::endl;

    std::cout << "Authenticating as sa..." << std::endl;
    if (!conn->Authenticate("sa", "DevPassword123!", "TestDB")) {
        std::cerr << "Auth failed: " << conn->GetLastError() << std::endl;
        return 1;
    }
    std::cout << "Authenticated!" << std::endl;

    std::cout << "Connection state: " << ConnectionStateToString(conn->GetState()) << std::endl;

    std::cout << "Executing batch: SELECT 1 AS test_value" << std::endl;
    if (!conn->ExecuteBatch("SELECT 1 AS test_value")) {
        std::cerr << "ExecuteBatch failed: " << conn->GetLastError() << std::endl;
        return 1;
    }
    std::cout << "Batch sent!" << std::endl;

    // Try to receive a packet
    std::cout << "Waiting for response..." << std::endl;
    auto* socket = conn->GetSocket();
    if (!socket) {
        std::cerr << "No socket!" << std::endl;
        return 1;
    }

    TdsPacket packet;
    std::cout << "Calling ReceivePacket with 5 second timeout..." << std::endl;
    if (!socket->ReceivePacket(packet, 5000)) {
        std::cerr << "ReceivePacket failed (timeout or error)" << std::endl;
        return 1;
    }

    std::cout << "Received packet!" << std::endl;
    std::cout << "  Type: 0x" << std::hex << (int)packet.GetType() << std::dec << std::endl;
    std::cout << "  Length: " << packet.GetLength() << std::endl;
    std::cout << "  Payload size: " << packet.GetPayload().size() << std::endl;

    // Print first 64 bytes of payload
    auto& payload = packet.GetPayload();
    std::cout << "  First bytes: ";
    for (size_t i = 0; i < std::min(payload.size(), (size_t)64); i++) {
        printf("%02x ", payload[i]);
    }
    std::cout << std::endl;

    return 0;
}
