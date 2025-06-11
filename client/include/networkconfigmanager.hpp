#include <string>
#include <guiddef.h>
#include <cstdint>
#include <iphlpapi.h>

namespace NetworkConstants
{
inline constexpr const wchar_t* INTERFACE_NAME = L"PeerBridge";
inline constexpr const wchar_t* TUNNEL_TYPE = L"WINTUN";

inline constexpr char const* NET_MASK = "255.255.255.0";
inline constexpr char const* MULTICAST_SUBNET_RANGE = "224.0.0.0/4";

inline constexpr uint8_t START_IP_INDEX = 1;
inline constexpr uint8_t BASE_IP_INDEX = 0;
}

class NetworkConfigManager
{
public:
    enum class RouteConfigApproach : uint8_t { GENERIC_ROUTE, FALLBACK_ROUTE_ALL, FAILED };

    struct SetupConfig
    {
        /* THIS VALUES WILL ONLY UPDATE WHEN CONNECTION IS NOT YET ESTABLISHED */

        const std::string IP_SPACE;
        const GUID ADAPTER_GUID;

        static SetupConfig loadConfig();
        // Following function is unused for now
        // TODO: Implement later
        // static bool saveConfig();
    };

    struct ConnectionConfig
    {
        // TODO: TO BE MODIFIED FOR *1
        uint8_t selfIndex;
        // TODO: TO BE MODIFIED FOR *1
        // For now, it will always equal 1
        std::string peerVirtualIp;
    };

    // TODO: Once config file implemented,
    // Do static FILE_PATH, staic SET_FILE_PATH or something similar

    NetworkConfigManager();

    bool configureInterface(const ConnectionConfig&);
    bool setupRouting(const ConnectionConfig&);
    void setupFirewall();

    void resetInterfaceConfiguration(const std::string&);
    bool removeRouting(const std::string&);
    void removeFirewall();

    void setNarrowAlias(const std::string&);

private:
    RouteConfigApproach routeApproach = RouteConfigApproach::GENERIC_ROUTE;
    std::string narrowAlias;
    SetupConfig setupConfig;

    bool executeNetshCommand(const std::string& command);

};