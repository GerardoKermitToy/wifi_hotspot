#include "hotspot/localization.hpp"
#include "hotspot/hotspot_service.hpp"

#include <windows.h>
#include <wlanapi.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>
#include <winrt/Windows.Devices.WiFi.h>
#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace hotspot {
namespace {
using namespace winrt;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::NetworkOperators;
using Windows::Foundation::Metadata::ApiInformation;

std::wstring GuidText(const guid& value) { return std::wstring(to_hstring(value)); }
[[noreturn]] void Fail(const std::wstring& message) { throw hresult_error(E_FAIL, message); }

std::wstring CapabilityText(TetheringCapability value) {
    switch (value) {
    case TetheringCapability::Enabled: return L"";
    case TetheringCapability::DisabledByGroupPolicy: return Tr(L"Мобильный хот-спот запрещён групповой политикой Windows. Обратитесь к администратору.", L"Mobile hotspot is disabled by Windows Group Policy. Contact your administrator.");
    case TetheringCapability::DisabledByHardwareLimitation: return Tr(L"Драйвер или Wi-Fi-адаптер не поддерживает мобильный хот-спот. Проверьте драйвер производителя.", L"The driver or Wi-Fi adapter does not support mobile hotspot. Check the manufacturer’s driver.");
    case TetheringCapability::DisabledByOperator: return Tr(L"Раздача Интернета запрещена оператором.", L"Internet sharing is disabled by your network operator.");
    case TetheringCapability::DisabledBySku: return Tr(L"Раздача недоступна в этой редакции Windows.", L"Internet sharing is unavailable in this edition of Windows.");
    case TetheringCapability::DisabledByRequiredAppNotInstalled: return Tr(L"Windows требует приложение оператора для раздачи Интернета.", L"Windows requires the network operator’s app for Internet sharing.");
    case TetheringCapability::DisabledBySystemCapability: return Tr(L"Windows не предоставила право wiFiControl. Проверьте ограничения устройства и политики безопасности.", L"Windows did not grant wiFiControl access. Check device restrictions and security policies.");
    default: return Tr(L"Windows не разрешает раздачу через выбранный источник. Проверьте подключение и службу мобильного хот-спота (icssvc).", L"Windows does not allow sharing the selected connection. Check the connection and mobile hotspot service (icssvc).");
    }
}
std::wstring OperationText(TetheringOperationStatus status) {
    switch (status) {
    case TetheringOperationStatus::Success: return Tr(L"Готово", L"Done");
    case TetheringOperationStatus::WiFiDeviceOff: return Tr(L"Включите Wi-Fi и отключите режим «В самолёте».", L"Turn on Wi-Fi and turn off airplane mode.");
    case TetheringOperationStatus::MobileBroadbandDeviceOff: return Tr(L"Модем источника Интернета выключен.", L"The mobile broadband device providing Internet access is off.");
    case TetheringOperationStatus::EntitlementCheckTimeout: return Tr(L"Оператор не ответил на проверку разрешения раздачи.", L"The network operator did not respond to the Internet sharing entitlement check.");
    case TetheringOperationStatus::EntitlementCheckFailure: return Tr(L"Оператор не разрешил раздачу Интернета.", L"The network operator did not allow Internet sharing.");
    case TetheringOperationStatus::OperationInProgress: return Tr(L"Windows уже выполняет другую операцию с хот-спотом. Дождитесь её завершения.", L"Windows is already performing another hotspot operation. Wait for it to finish.");
    case TetheringOperationStatus::BluetoothDeviceOff: return Tr(L"Windows сообщает, что Bluetooth-устройство выключено.", L"Windows reports that the Bluetooth device is off.");
    case TetheringOperationStatus::NetworkLimitedConnectivity: return Tr(L"У источника ограниченное подключение. Проверьте доступ в Интернет.", L"The source has limited connectivity. Check Internet access.");
    case TetheringOperationStatus::AlreadyOn: return Tr(L"Хот-спот уже включён. Чужая сессия не будет остановлена или перенастроена.", L"The hotspot is already on. An existing session will not be stopped or reconfigured.");
    case TetheringOperationStatus::RadioRestriction: return Tr(L"Драйвер или региональные ограничения запрещают выбранный радиорежим. Попробуйте 2,4 ГГц / WPA2.", L"The driver or regional restrictions do not allow this radio mode. Try 2.4 GHz / WPA2.");
    case TetheringOperationStatus::BandInterference: return Tr(L"Диапазон конфликтует с текущим Wi-Fi-подключением. Выберите другой диапазон или явно включите приоритет точки доступа.", L"This band conflicts with the current Wi-Fi connection. Choose another band or explicitly prioritize the hotspot.");
    default: return Tr(L"Windows не смогла выполнить операцию. Проверьте драйвер, подключение и службу icssvc.", L"Windows could not complete the operation. Check the driver, connection, and icssvc service.");
    }
}
void CheckResult(const NetworkOperatorTetheringOperationResult& result) {
    if (result.Status() == TetheringOperationStatus::Success) return;
    auto message = OperationText(result.Status());
    message += Tr(L" (код ", L" (code ") + std::to_wstring(static_cast<int>(result.Status())) + L")";
    const auto detail = result.AdditionalErrorMessage();
    if (!detail.empty()) message += L"\n" + std::wstring(detail);
    Fail(message);
}
State ToState(TetheringOperationalState state) {
    switch (state) {
    case TetheringOperationalState::Off: return State::Off;
    case TetheringOperationalState::On: return State::On;
    case TetheringOperationalState::InTransition: return State::InTransition;
    default: return State::Unknown;
    }
}
TetheringWiFiBand ToBand(Band band) {
    switch (band) {
    case Band::Ghz24: return TetheringWiFiBand::TwoPointFourGigahertz;
    case Band::Ghz5: return TetheringWiFiBand::FiveGigahertz;
    case Band::Ghz6: return TetheringWiFiBand::SixGigahertz;
    default: return TetheringWiFiBand::Auto;
    }
}
TetheringWiFiAuthenticationKind ToAuth(Security security) {
    switch (security) {
    case Security::Wpa3Transition: return TetheringWiFiAuthenticationKind::Wpa3TransitionMode;
    case Security::Wpa3: return TetheringWiFiAuthenticationKind::Wpa3;
    default: return TetheringWiFiAuthenticationKind::Wpa2;
    }
}
std::wstring AdapterAlias(const guid& id, const std::wstring& fallback) {
    NET_LUID luid{};
    WCHAR alias[IF_MAX_STRING_SIZE + 1]{};
    if (ConvertInterfaceGuidToLuid(reinterpret_cast<const GUID*>(&id), &luid) == NO_ERROR &&
        ConvertInterfaceLuidToAlias(&luid, alias, static_cast<SIZE_T>(std::size(alias))) == NO_ERROR) return alias;
    return fallback;
}
struct WlanHandle {
    HANDLE value = nullptr;
    ~WlanHandle() { if (value) WlanCloseHandle(value, nullptr); }
};
struct WlanList {
    WLAN_INTERFACE_INFO_LIST* value = nullptr;
    ~WlanList() { if (value) WlanFreeMemory(value); }
};
}

struct HotspotService::Impl {
    Snapshot snapshot;
    std::map<std::wstring, NetworkAdapter> adapters;
    std::map<std::wstring, ConnectionProfile> sources;
    NetworkOperatorTetheringManager manager{nullptr};

    NetworkOperatorTetheringManager MakeManager(const std::wstring& adapterId, const std::wstring& sourceId) {
        const auto adapter = adapters.find(adapterId);
        const auto source = sources.find(sourceId);
        if (adapter == adapters.end()) Fail(Tr(L"Выбранный Wi-Fi-адаптер недоступен. Нажмите «Обновить».", L"The selected Wi-Fi adapter is unavailable. Click Refresh.") );
        if (source == sources.end()) Fail(Tr(L"Источник Интернета недоступен. Подключите Ethernet или Wi-Fi и нажмите «Обновить».", L"The Internet source is unavailable. Connect Ethernet or Wi-Fi and click Refresh.") );
        const auto capability = NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(source->second);
        if (capability != TetheringCapability::Enabled) Fail(CapabilityText(capability));
        // Important: NEVER use the one-argument overload. It lets Windows pick another adapter.
        return NetworkOperatorTetheringManager::CreateFromConnectionProfile(source->second, adapter->second);
    }
    Snapshot ReadStatus() {
        if (!manager) return snapshot;
        snapshot.state = ToState(manager.TetheringOperationalState());
        if (snapshot.state == State::Off) snapshot.owned = false;
        snapshot.clientCount = manager.ClientCount();
        snapshot.maxClients = manager.MaxClientCount();
        snapshot.clients.clear();
        if (snapshot.state == State::On) {
            for (const auto& client : manager.GetTetheringClients()) {
                ClientInfo info{std::wstring(client.MacAddress()), L""};
                for (const auto& host : client.HostNames()) {
                    if (!info.name.empty()) info.name += L", ";
                    info.name += host.DisplayName();
                }
                snapshot.clients.push_back(std::move(info));
            }
        }
        return snapshot;
    }
};

HotspotService::HotspotService() : impl_(std::make_unique<Impl>()) {
    if (!ApiInformation::IsTypePresent(L"Windows.Networking.NetworkOperators.NetworkOperatorTetheringSessionAccessPointConfiguration")) {
        Fail(Tr(L"Нужна актуальная Windows 11 (24H2, сборка 26100 или новее) с API сессионного хот-спота. Старый Hosted Network не используется.", L"Windows 11 24H2 (build 26100 or later) with the per-session hotspot API is required. Legacy Hosted Network is not used."));
    }
}
HotspotService::~HotspotService() {
    // Normal GUI shutdown calls Stop explicitly and reports failure. This is a last-resort
    // cleanup for CLI exceptions, always on the same worker/MTA thread.
    if (impl_ && impl_->snapshot.owned && impl_->manager) {
        try { impl_->manager.StopTetheringAsync().get(); } catch (...) { }
    }
}

Snapshot HotspotService::Discover(const std::wstring& adapterId, const std::wstring& sourceId) {
    auto& p = *impl_;
    // Keep the exact live manager and adapter binding for the life of our session.
    if (p.manager && (p.ReadStatus().state == State::On || p.snapshot.state == State::InTransition)) return p.snapshot;
    p.manager = nullptr;
    p.adapters.clear();
    p.sources.clear();
    p.snapshot = {};

    // Native WLAN enumerates interfaces without scanning nearby networks or requiring location.
    WlanHandle wlan;
    DWORD negotiated = 0;
    auto error = WlanOpenHandle(2, nullptr, &negotiated, &wlan.value);
    if (error != ERROR_SUCCESS) {
        Fail(Tr(L"Не удалось открыть службу WLAN (код ", L"Could not open the WLAN service (code ") + std::to_wstring(error) + Tr(L"). Включите службу автонастройки WLAN (WlanSvc).", L"). Enable the WLAN AutoConfig service (WlanSvc).") );
    }
    WlanList interfaces;
    error = WlanEnumInterfaces(wlan.value, nullptr, &interfaces.value);
    if (error != ERROR_SUCCESS) Fail(Tr(L"Не удалось перечислить Wi-Fi-адаптеры (код ", L"Could not enumerate Wi-Fi adapters (code ") + std::to_wstring(error) + L").");

    // Profiles cover connected interfaces; WiFiAdapter supplies disconnected radios.
    std::map<std::wstring, NetworkAdapter> winrtAdapters;
    const auto profiles = NetworkInformation::GetConnectionProfiles();
    for (const auto& profile : profiles) {
        if (const auto a = profile.NetworkAdapter()) winrtAdapters.insert_or_assign(GuidText(a.NetworkAdapterId()), a);
    }
    bool missingAdapter = false;
    for (DWORD i = 0; i < interfaces.value->dwNumberOfItems; ++i) {
        if (!winrtAdapters.contains(GuidText(interfaces.value->InterfaceInfo[i].InterfaceGuid))) missingAdapter = true;
    }
    if (missingAdapter) {
        // No scans and no RequestAccess prompt. Some policy-managed machines deny this;
        // still display native adapters, but never substitute another interface.
        try {
            for (const auto& wifi : Windows::Devices::WiFi::WiFiAdapter::FindAllAdaptersAsync().get()) {
                const auto a = wifi.NetworkAdapter();
                winrtAdapters.insert_or_assign(GuidText(a.NetworkAdapterId()), a);
            }
        } catch (...) {
            p.snapshot.notice = Tr(L"Windows не предоставила WinRT-доступ к части адаптеров. Проверьте разрешение геолокации / Wi-Fi: Параметры → Конфиденциальность и безопасность → Расположение. Политики и разрешения приложение не изменяет.", L"Windows did not grant WinRT access to some adapters. Check location / Wi-Fi access: Settings → Privacy & security → Location. This app does not change policies or permissions.");
        }
    }
    for (DWORD i = 0; i < interfaces.value->dwNumberOfItems; ++i) {
        const auto& native = interfaces.value->InterfaceInfo[i];
        const auto id = GuidText(native.InterfaceGuid);
        const auto found = winrtAdapters.find(id);
        AdapterInfo info;
        info.id = id;
        info.description = native.strInterfaceDescription;
        info.name = AdapterAlias(native.InterfaceGuid, info.description);
        info.available = found != winrtAdapters.end();
        info.connectionState = native.isState == wlan_interface_state_connected ? Tr(L"Подключён", L"Connected") :
                               native.isState == wlan_interface_state_disconnected ? Tr(L"Не подключён к сети", L"Not connected") :
                               native.isState == wlan_interface_state_not_ready ? Tr(L"Не готов", L"Not ready") : Tr(L"Доступен", L"Available");
        if (info.available) p.adapters.emplace(id, found->second);
        p.snapshot.adapters.push_back(std::move(info));
    }

    const auto defaultProfile = NetworkInformation::GetInternetConnectionProfile();
    std::wstring defaultId;
    if (defaultProfile && defaultProfile.NetworkAdapter()) defaultId = GuidText(defaultProfile.NetworkAdapter().NetworkAdapterId());
    for (const auto& profile : profiles) {
        const auto a = profile.NetworkAdapter();
        if (!a || profile.GetNetworkConnectivityLevel() == NetworkConnectivityLevel::None) continue;
        SourceInfo info;
        info.id = GuidText(a.NetworkAdapterId());
        if (p.sources.contains(info.id)) continue;
        info.name = AdapterAlias(a.NetworkAdapterId(), std::wstring(profile.ProfileName()));
        info.internet = profile.GetNetworkConnectivityLevel() == NetworkConnectivityLevel::InternetAccess;
        info.isDefault = info.id == defaultId;
        p.sources.emplace(info.id, profile);
        p.snapshot.sources.push_back(std::move(info));
    }
    std::stable_sort(p.snapshot.sources.begin(), p.snapshot.sources.end(), [](const auto& a, const auto& b) {
        return a.isDefault > b.isDefault;
    });
    // An explicit selection is never replaced by a different adapter/source.
    if (!adapterId.empty()) p.snapshot.adapterId = adapterId;
    else if (!p.adapters.empty()) {
        const auto first = std::find_if(p.snapshot.adapters.begin(), p.snapshot.adapters.end(), [](const auto& a) { return a.available; });
        p.snapshot.adapterId = first->id;
    }
    if (!sourceId.empty()) p.snapshot.sourceId = sourceId;
    else if (!p.snapshot.sources.empty()) p.snapshot.sourceId = p.snapshot.sources.front().id;

    if (p.snapshot.adapters.empty()) p.snapshot.notice = Tr(L"Wi-Fi-адаптеры не найдены. Подключите адаптер и установите драйвер производителя.", L"No Wi-Fi adapters found. Connect an adapter and install the manufacturer’s driver.");
    else if (p.adapters.empty()) p.snapshot.notice = Tr(L"Wi-Fi-адаптер найден, но Windows не предоставляет его NetworkAdapter. Проверьте разрешение геолокации / Wi-Fi и политики расположения в Windows, затем нажмите «Обновить».", L"A Wi-Fi adapter was found, but Windows did not provide its NetworkAdapter. Check location / Wi-Fi permissions and location policies in Windows, then click Refresh.");
    else if (p.sources.empty()) p.snapshot.notice = Tr(L"Нет активного источника подключения. Подключите Ethernet или Wi-Fi. Mobile Hotspot требует сетевой профиль; автономный SoftAP этим API не создаётся.", L"No active source connection. Connect Ethernet or Wi-Fi. Mobile Hotspot requires a network profile; this API does not create a standalone SoftAP.");
    if (!p.snapshot.adapterId.empty() && !p.snapshot.sourceId.empty()) {
        try {
            p.manager = p.MakeManager(p.snapshot.adapterId, p.snapshot.sourceId);
            p.ReadStatus();
            if (p.snapshot.state == State::On) p.snapshot.notice = Tr(L"Хот-спот уже включён другим приложением или Windows. Выключите его в параметрах Windows перед созданием новой сессии.", L"The hotspot was already started by another app or Windows. Turn it off in Windows Settings before creating a new session.");
        } catch (...) { p.snapshot.notice = CurrentExceptionMessage(); }
    }
    return p.snapshot;
}
Snapshot HotspotService::Poll() { return impl_->ReadStatus(); }

Snapshot HotspotService::Start(const Settings& settings, const std::wstring& adapterId, const std::wstring& sourceId) {
    const auto errors = Validate(settings);
    if (!errors.empty()) Fail(errors.front());
    auto& p = *impl_;
    if (p.manager) {
        const auto state = p.ReadStatus().state;
        if (state != State::Off) Fail(Tr(L"Нельзя начать новую сессию: ", L"Cannot start a new session: ") + StateLabel(state) + Tr(L". Существующая точка доступа не будет изменена.", L". The existing hotspot will not be changed."));
    }
    auto manager = p.MakeManager(adapterId, sourceId);
    if (manager.TetheringOperationalState() != TetheringOperationalState::Off) {
        Fail(Tr(L"Хот-спот уже работает или меняет состояние. Сначала выключите существующую сессию.", L"The hotspot is already running or changing state. Turn off the existing session first."));
    }
    NetworkOperatorTetheringSessionAccessPointConfiguration configuration;
    configuration.Ssid(settings.ssid);
    configuration.Passphrase(settings.passphrase);
    const auto band = ToBand(settings.band);
    const auto auth = ToAuth(settings.security);
    if (!configuration.IsBandSupportedAsync(band).get()) Fail(Tr(L"Windows/драйвер не поддерживает хот-спот в диапазоне «", L"Windows / the driver does not support a hotspot on the “") + BandLabel(settings.band) + Tr(L"». Выберите другой диапазон.", L"” band. Choose another band."));
    if (!configuration.IsAuthenticationKindSupportedAsync(auth).get()) Fail(Tr(L"Windows/драйвер не поддерживает «", L"Windows / the driver does not support “") + SecurityLabel(settings.security) + Tr(L"» для хот-спота. Защита не будет понижена автоматически.", L"” for the hotspot. Security will not be downgraded automatically."));
    configuration.Band(band);
    configuration.AuthenticationKind(auth);
    configuration.PerformancePriority(settings.prioritizeHotspot ? TetheringWiFiPerformancePriority::TetheringOverStation : TetheringWiFiPerformancePriority::Default);
    // Per-session only. NEVER call ConfigureAccessPointAsync, toggle timeout settings,
    // disable interfaces, edit registry/ICS, or stop an existing hotspot to start ours.
    const auto result = manager.StartTetheringAsync(configuration).get();
    CheckResult(result);
    p.manager = manager;
    p.snapshot.adapterId = adapterId;
    p.snapshot.sourceId = sourceId;
    p.snapshot.owned = true;
    p.snapshot.notice = Tr(L"Сессионные настройки: сохранённые имя и пароль хот-спота Windows не изменялись.", L"Session-only settings: the saved Windows hotspot name and password were not changed.");
    return p.ReadStatus();
}
Snapshot HotspotService::Stop() {
    auto& p = *impl_;
    if (!p.manager || !p.snapshot.owned) Fail(Tr(L"Приложение не создавало эту сессию и не будет её останавливать.", L"This app did not create this session and will not stop it."));
    if (p.manager.TetheringOperationalState() != TetheringOperationalState::Off) {
        CheckResult(p.manager.StopTetheringAsync().get());
    }
    // Keep ownership on a failed/indeterminate stop, so the UI can retry and
    // shutdown cleanup still knows which session belongs to this process.
    if (p.manager.TetheringOperationalState() != TetheringOperationalState::Off) {
        Fail(Tr(L"Windows ещё не подтвердила выключение точки доступа. Дождитесь завершения операции и повторите выключение.", L"Windows has not confirmed that the hotspot is off. Wait for the operation to finish and try stopping it again."));
    }
    p.snapshot.owned = false;
    p.snapshot.notice = Tr(L"Точка доступа остановлена. Сохранённые настройки хот-спота Windows не изменялись.", L"Hotspot stopped. The saved Windows hotspot settings were not changed.");
    return p.ReadStatus();
}
bool HotspotService::OwnsSession() const noexcept { return impl_->snapshot.owned; }
std::wstring HotspotService::PersistentConfigurationSignature() const {
    if (!impl_->manager) Fail(Tr(L"Сначала выберите доступные адаптер и источник подключения.", L"Select an available adapter and source connection first."));
    const auto c = impl_->manager.GetCurrentAccessPointConfiguration();
    // A length-delimited in-memory signature; must never be displayed or persisted.
    const auto ssid = std::wstring(c.Ssid());
    const auto password = std::wstring(c.Passphrase());
    return std::to_wstring(ssid.size()) + L":" + ssid + std::to_wstring(password.size()) + L":" + password +
        L":" + std::to_wstring(static_cast<int>(c.Band())) + L":" + std::to_wstring(static_cast<int>(c.AuthenticationKind()));
}
std::wstring CurrentExceptionMessage() {
    try { throw; }
    catch (const hresult_error& e) {
        std::wostringstream out;
        out << e.message().c_str() << L"\nHRESULT 0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << static_cast<unsigned long>(e.code().value);
        if (e.code() == E_ACCESSDENIED) out << Tr(L"\nWindows отказала в доступе. Проверьте политики безопасности и разрешения управления Wi-Fi.", L"\nWindows denied access. Check security policies and Wi-Fi control permissions.");
        return out.str();
    } catch (const std::exception& e) { return std::wstring(to_hstring(e.what())); }
    catch (...) { return Tr(L"Неизвестная ошибка приложения.", L"Unknown application error."); }
}
} // namespace hotspot
