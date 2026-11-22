#pragma once

#include "domain.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hotspot {

struct AdapterInfo {
    std::wstring id;
    std::wstring name;
    std::wstring description;
    std::wstring connectionState;
    bool available = false; // A native WLAN interface AND a matching WinRT NetworkAdapter.
};
struct SourceInfo {
    std::wstring id;
    std::wstring name;
    bool internet = false;
    bool isDefault = false;
};
struct ClientInfo {
    std::wstring mac;
    std::wstring name;
};
struct Snapshot {
    std::vector<AdapterInfo> adapters;
    std::vector<SourceInfo> sources;
    std::wstring adapterId;
    std::wstring sourceId;
    State state = State::Unknown;
    bool owned = false;
    std::uint32_t clientCount = 0;
    std::uint32_t maxClients = 0;
    std::vector<ClientInfo> clients;
    std::wstring notice;
};

// Construct, call, and destroy on ONE MTA thread. No calls on the GUI thread.
class HotspotService {
public:
    HotspotService();
    ~HotspotService();
    HotspotService(const HotspotService&) = delete;
    HotspotService& operator=(const HotspotService&) = delete;

    Snapshot Discover(const std::wstring& adapterId = {}, const std::wstring& sourceId = {});
    Snapshot Poll();
    Snapshot Start(const Settings& settings, const std::wstring& adapterId, const std::wstring& sourceId);
    Snapshot Stop();
    bool OwnsSession() const noexcept;
    // Only for the explicit hardware test. Never prints or writes the configuration.
    std::wstring PersistentConfigurationSignature() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::wstring CurrentExceptionMessage(); // Call inside a catch block; includes HRESULT.
} // namespace hotspot
