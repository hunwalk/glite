// The logi module's library: lists Logitech devices by talking HID++ 2.0 to them, as Solaar does
// (https://github.com/pwr-Solaar/Solaar). Requests go straight to the devices; G HUB doesn't have
// to be running, and it may keep running alongside.
//
// rkm_logi_scan() runs one scan on a worker thread and posts, for the scan's id:
//   device  tab-separated: name, kind, connection, battery percent (-1 unknown or none), battery
//           state ('' unknown), "1" when it has LIGHTSYNC per-key lighting, its address for
//           rkm_logi_light() (HID path|device index; '' when it has none)
//   done    '' when the scan finished, else why it couldn't run
//
// rkm_logi_light(address, color) paints a keyboard one colour, or gives its lighting back (color
// -1), on the same worker thread, and posts done ('' or why not) for its id.
//
// On Windows every HID++ interface shows up as two HID collections (usage page 0xFF00): usage 1
// for short reports (7 bytes) and usage 2 for long ones (20 bytes). Requests are sent as long
// reports; answers come back long, but a receiver's "no device in that slot" error comes back
// short, so both collections are read.
//
// Newer headsets (the G522) speak Logitech's Centurion protocol on usage page 0xFFA0 instead
// (class Centurion, ported from HeadsetControl). Devices with other vendor-defined collections
// are listed by name only.
#include "rk_module_logi.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::mutex posting;
std::mutex state;
rk_post_fn post = nullptr;
std::thread worker;
std::atomic<bool> running{false};
std::atomic<bool> cancelled{false};
int64_t nextId = 1;

void send(int64_t id, const char *kind, const std::string &data)
{
    std::lock_guard<std::mutex> lock(posting);
    if (!cancelled && post != nullptr) {
        post("logi", id, kind, data.data(), data.size());
    }
}

/** Device kinds of feature 0x0005 (DEVICE_NAME), function 2. */
std::string kindName(uint8_t kind)
{
    static const char *const names[] = {"keyboard", "remote control", "numpad", "mouse", "trackpad",
        "trackball", "presenter", "receiver", "headset", "webcam", "steering wheel", "joystick",
        "gamepad", "dock", "speaker", "microphone"};
    return kind < sizeof names / sizeof names[0] ? names[kind] : "device";
}

/** Tabs and line breaks would break the event's fields. */
std::string clean(std::string text)
{
    for (char &c : text) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

} // namespace

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace {

constexpr USHORT kLogitech = 0x046D;
constexpr uint8_t kSoftwareId = 0x0A; // marks our requests' answers; notifications use 0
constexpr DWORD kTimeoutMs = 800;
// LIGHTSYNC features (the PRO X TKL has all three)
constexpr uint16_t kRgbEffects = 0x8071;      // RGB_EFFECTS: who drives the LEDs, zone effects
constexpr uint16_t kPerKeyLighting = 0x8081;  // PER_KEY_LIGHTING_V2: colours per key
constexpr uint16_t kOnboardProfiles = 0x8100; // ONBOARD_PROFILES: onboard or host mode

std::string utf8(const wchar_t *text)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return "";
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

/** One HID++ interface: its short and long collections, children of the same device node. */
struct Interface {
    std::wstring shortPath;
    std::wstring longPath;
    DWORD shortInput = 0;
    DWORD longInput = 0;
    DWORD longOutput = 0;
    std::string product;
    // Has a vendor-defined collection other than HID++'s, e.g. the G522 headset (0xFF13, 0xFFA0)
    bool otherVendor = false;
    // Centurion (usage page 0xFFA0, see class Centurion)
    std::wstring centurionPath;
    DWORD centurionInput = 0;
    DWORD centurionOutput = 0;
    uint8_t centurionReportId = 0;
};

/** The report ID of a collection's first output report; 0 when it has none (or no IDs). */
uint8_t outputReportId(PHIDP_PREPARSED_DATA preparsed, const HIDP_CAPS &caps)
{
    USHORT count = caps.NumberOutputValueCaps;
    if (count == 0) {
        return 0;
    }
    std::vector<HIDP_VALUE_CAPS> values(count);
    if (HidP_GetValueCaps(HidP_Output, values.data(), &count, preparsed) != HIDP_STATUS_SUCCESS || count == 0) {
        return 0;
    }
    return static_cast<uint8_t>(values[0].ReportID);
}

std::vector<Interface> interfaces()
{
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::map<DEVINST, Interface> byParent;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &iface); i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        if (size == 0) {
            continue;
        }
        std::vector<unsigned char> buffer(size);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof info;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, &info)) {
            continue;
        }
        // No access rights needed to read attributes and capabilities.
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof attributes;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps{};
        bool ok = HidD_GetAttributes(h, &attributes) && attributes.VendorID == kLogitech
            && HidD_GetPreparsedData(h, &preparsed) && HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
        uint8_t reportId = ok ? outputReportId(preparsed, caps) : 0;
        if (preparsed != nullptr) {
            HidD_FreePreparsedData(preparsed);
        }
        if (ok && caps.UsagePage >= 0xFF00) {
            DEVINST parent = 0;
            CM_Get_Parent(&parent, info.DevInst, 0);
            Interface &found = byParent[parent];
            if (caps.UsagePage == 0xFFA0 && caps.Usage == 1 && caps.OutputReportByteLength >= 64) {
                found.otherVendor = true;
                found.centurionPath = detail->DevicePath;
                found.centurionInput = caps.InputReportByteLength;
                found.centurionOutput = caps.OutputReportByteLength;
                found.centurionReportId = reportId;
            } else if (caps.UsagePage != 0xFF00 || (caps.Usage != 1 && caps.Usage != 2)) {
                found.otherVendor = true;
            } else if (caps.Usage == 1) {
                found.shortPath = detail->DevicePath;
                found.shortInput = caps.InputReportByteLength;
            } else {
                found.longPath = detail->DevicePath;
                found.longInput = caps.InputReportByteLength;
                found.longOutput = caps.OutputReportByteLength;
            }
            wchar_t product[128] = {};
            if (found.product.empty() && HidD_GetProductString(h, product, sizeof product)) {
                found.product = utf8(product);
            }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);

    std::vector<Interface> out;
    for (auto &[parent, found] : byParent) {
        if ((!found.longPath.empty() && found.longOutput >= 20) || !found.centurionPath.empty() || found.otherVendor) {
            out.push_back(found);
        }
    }
    return out;
}

/** An open HID collection that always has one read in flight. */
class Pipe {
public:
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    bool reading = false;

    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;

    Pipe(const std::wstring &path, DWORD inputLength)
    {
        length = inputLength < sizeof buffer ? inputLength : sizeof buffer;
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            arm();
        }
    }

    ~Pipe()
    {
        if (handle != INVALID_HANDLE_VALUE) {
            if (reading) {
                CancelIoEx(handle, &overlapped);
                DWORD n = 0;
                GetOverlappedResult(handle, &overlapped, &n, TRUE); // the buffer must outlive the read
            }
            CloseHandle(handle);
        }
        CloseHandle(overlapped.hEvent);
    }

    bool isOpen() const { return handle != INVALID_HANDLE_VALUE; }

    /** After the read's event fired: takes the report and starts the next read. */
    bool take(std::vector<uint8_t> &report)
    {
        DWORD n = 0;
        bool ok = GetOverlappedResult(handle, &overlapped, &n, FALSE);
        reading = false;
        if (ok) {
            report.assign(buffer, buffer + n);
        }
        arm();
        return ok;
    }

    bool write(const std::vector<uint8_t> &report)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = WriteFile(handle, report.data(), static_cast<DWORD>(report.size()), &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, kTimeoutMs) != WAIT_OBJECT_0) {
                CancelIoEx(handle, &ov);
            }
            ok = GetOverlappedResult(handle, &ov, &n, TRUE);
        }
        CloseHandle(ov.hEvent);
        return ok && n == report.size();
    }

private:
    uint8_t buffer[64] = {};
    DWORD length = 0;

    void arm()
    {
        ResetEvent(overlapped.hEvent);
        DWORD n = 0;
        reading = ReadFile(handle, buffer, length, &n, &overlapped) || GetLastError() == ERROR_IO_PENDING;
    }
};

/** HID++ 2.0 requests to the devices behind one interface. */
class Link {
public:
    explicit Link(const Interface &iface) : output(iface.longOutput)
    {
        longPipe = std::make_unique<Pipe>(iface.longPath, iface.longInput);
        if (!iface.shortPath.empty()) {
            shortPipe = std::make_unique<Pipe>(iface.shortPath, iface.shortInput);
        }
    }

    bool isOpen() const { return longPipe->isOpen(); }

    /** Sends a request; true with the answer's 16 parameter bytes in `reply`. */
    bool request(uint8_t device, uint8_t feature, uint8_t function, std::initializer_list<uint8_t> params,
        uint8_t reply[16])
    {
        std::vector<uint8_t> report(output, 0);
        const uint8_t tag = static_cast<uint8_t>((function << 4) | kSoftwareId);
        report[0] = 0x11;
        report[1] = device;
        report[2] = feature;
        report[3] = tag;
        size_t at = 4;
        for (uint8_t p : params) {
            if (at < report.size()) {
                report[at++] = p;
            }
        }
        if (!longPipe->write(report)) {
            return false;
        }

        const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
        std::vector<uint8_t> r;
        while (!cancelled) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                return false;
            }
            HANDLE events[2];
            Pipe *pipes[2];
            DWORD count = 0;
            for (Pipe *pipe : {longPipe.get(), shortPipe.get()}) {
                if (pipe != nullptr && pipe->isOpen() && pipe->reading) {
                    events[count] = pipe->overlapped.hEvent;
                    pipes[count++] = pipe;
                }
            }
            if (count == 0) {
                return false;
            }
            DWORD woke = WaitForMultipleObjects(count, events, FALSE, static_cast<DWORD>(deadline - now));
            if (woke >= WAIT_OBJECT_0 + count) {
                return false; // timed out (or failed)
            }
            if (!pipes[woke - WAIT_OBJECT_0]->take(r) || r.size() < 7 || r[1] != device) {
                continue;
            }
            if (r[0] == 0x11 && r.size() >= 20 && r[2] == feature && r[3] == tag) {
                std::memcpy(reply, &r[4], 16);
                return true;
            }
            if ((r[0] == 0x11 && r[2] == 0xFF && r[3] == feature && r[4] == tag)     // HID++ 2.0 error
                || (r[0] == 0x10 && r[2] == 0x8F && r[3] == feature && r[4] == tag)) { // HID++ 1.0 error
                return false;
            }
            // anything else is someone else's traffic (G HUB, notifications): keep waiting
        }
        return false;
    }

    /** The index of a feature on the device (root feature, function 0); 0 when it has none. */
    uint8_t feature(uint8_t device, uint16_t id)
    {
        uint8_t r[16];
        if (!request(device, 0x00, 0, {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id & 0xFF)}, r)) {
            return 0;
        }
        return r[0];
    }

private:
    std::unique_ptr<Pipe> longPipe;
    std::unique_ptr<Pipe> shortPipe;
    DWORD output;
};

/** Percent left from a battery voltage in mV: an approximate Li-ion curve, as Solaar uses. */
int percentFromVoltage(int millivolts)
{
    static const int curve[][2] = {{4186, 100}, {4067, 90}, {3989, 80}, {3922, 70}, {3859, 60}, {3811, 50},
        {3778, 40}, {3751, 30}, {3717, 20}, {3671, 10}, {3646, 5}, {3579, 2}};
    for (const auto &point : curve) {
        if (millivolts >= point[0]) {
            return point[1];
        }
    }
    return 0;
}

/**
 * The battery's charge (percent, -1 when unknown) and state, from whichever battery feature the
 * device has. Features and their meaning as in Solaar.
 */
void battery(Link &link, uint8_t device, int &percent, std::string &status)
{
    uint8_t r[16];
    uint8_t index = link.feature(device, 0x1004); // UNIFIED_BATTERY: get_status
    if (index != 0 && link.request(device, index, 1, {}, r)) {
        static const char *const states[] = {"discharging", "charging", "charging slowly", "full", "charging error"};
        percent = r[0];
        if (percent == 0) { // some report only a level
            percent = (r[1] & 8) ? 90 : (r[1] & 4) ? 50 : (r[1] & 2) ? 20 : (r[1] & 1) ? 5 : -1;
        }
        status = r[2] < 5 ? states[r[2]] : "";
        return;
    }
    index = link.feature(device, 0x1000); // BATTERY_STATUS
    if (index != 0 && link.request(device, index, 0, {}, r)) {
        static const char *const states[] = {"discharging", "charging", "almost full", "full", "charging slowly",
            "invalid battery", "thermal error"};
        percent = r[0] > 0 && r[0] <= 100 ? r[0] : -1;
        status = r[2] < 7 ? states[r[2]] : "";
        return;
    }
    index = link.feature(device, 0x1001); // BATTERY_VOLTAGE: millivolts, flags
    if (index != 0 && link.request(device, index, 0, {}, r)) {
        percent = percentFromVoltage((r[0] << 8) | r[1]);
        status = (r[2] & 0x80) ? "charging" : "discharging";
        return;
    }
    index = link.feature(device, 0x1F20); // ADC_MEASUREMENT (headsets): millivolts, flags
    if (index != 0 && link.request(device, index, 0, {}, r) && (r[2] & 0x01)) {
        percent = percentFromVoltage((r[0] << 8) | r[1]);
        status = (r[2] & 0x02) ? "charging" : "discharging";
    }
}

/** Asks one device for its name and kind; false when nothing (HID++ 2.0) answers there. */
bool describe(Link &link, uint8_t device, std::string &name, std::string &kind)
{
    uint8_t r[16];
    // Ping (root feature, function 1): answers with the protocol version. A sleeping wireless
    // device can miss the first one while it wakes up, so it gets a second.
    if (!link.request(device, 0x00, 1, {0, 0, 0x5A}, r) && !link.request(device, 0x00, 1, {0, 0, 0x5A}, r)) {
        return false;
    }
    if (r[0] < 2) {
        return false;
    }
    uint8_t index = link.feature(device, 0x0005); // DEVICE_NAME
    if (index == 0 || !link.request(device, index, 0, {}, r)) {
        return true;
    }
    size_t length = r[0];
    while (name.size() < length) {
        if (!link.request(device, index, 1, {static_cast<uint8_t>(name.size())}, r)) {
            break;
        }
        for (int i = 0; i < 16 && name.size() < length; i++) {
            name += static_cast<char>(r[i]);
        }
    }
    if (link.request(device, index, 2, {}, r)) {
        kind = kindName(r[0]);
    }
    return true;
}

/**
 * Logitech's Centurion protocol, which newer headsets (G522, PRO X 2) speak on usage page 0xFFA0
 * instead of HID++. Ported from HeadsetControl (GPLv3, https://github.com/Sapd/HeadsetControl,
 * lib/devices/protocols/logitech_centurion_protocol.hpp):
 *
 *   frame    <prefix> <payload length + 1> <flags> <payload>, 64 bytes; the prefix is 50 23
 *            (G522) or 51 (PRO X 2), after the collection's report ID
 *   payload  <feature index> <function | software ID> <params…>, to the receiver itself
 *   bridged  <bridge index> <0x10 | software ID> <length: 2 bytes> 00 <feature> <function | sw>
 *            <params…>: the receiver's bridge feature carries it to the headset. The receiver
 *            acknowledges (function 1, our software ID), then passes on the headset's answer
 *            (function 1, software ID 0) with the same layout.
 */
class Centurion {
public:
    uint8_t bridge = 0; // the receiver's bridge feature index, once known

    explicit Centurion(const Interface &iface)
        : output(iface.centurionOutput),
          prefix(iface.centurionReportId == 0x51 ? std::vector<uint8_t>{0x51} : std::vector<uint8_t>{0x50, 0x23}),
          pipe(iface.centurionPath, iface.centurionInput)
    {
    }

    bool isOpen() const { return pipe.isOpen(); }

    /** A request to the receiver; true with the answer's parameters in `reply`. */
    bool direct(uint8_t feature, uint8_t function, std::initializer_list<uint8_t> params, std::vector<uint8_t> &reply)
    {
        const uint8_t tag = static_cast<uint8_t>(function | kRequestTag);
        std::vector<uint8_t> payload{feature, tag};
        payload.insert(payload.end(), params);
        if (!write(payload)) {
            return false;
        }
        return await([&](const std::vector<uint8_t> &p) {
            if (p.size() >= 2 && p[0] == feature && p[1] == tag) {
                reply.assign(p.begin() + 2, p.end());
                return kDone;
            }
            return p.size() >= 2 && p[0] == 0xFF && p[1] == feature ? kFailed : kWaiting;
        });
    }

    /** A request to the headset; false when it's off, out of range or says no. */
    bool bridged(uint8_t feature, uint8_t function, std::initializer_list<uint8_t> params, std::vector<uint8_t> &reply)
    {
        const uint8_t tag = static_cast<uint8_t>(function | kRequestTag);
        std::vector<uint8_t> message{0x00, feature, tag};
        message.insert(message.end(), params);
        std::vector<uint8_t> payload{bridge, static_cast<uint8_t>(0x10 | kRequestTag), 0x00,
            static_cast<uint8_t>(message.size())};
        payload.insert(payload.end(), message.begin(), message.end());
        if (!write(payload)) {
            return false;
        }
        return await([&](const std::vector<uint8_t> &p) {
            if (p.size() >= 2 && p[0] == 0xFF && p[1] == bridge) {
                return kFailed; // the receiver refused: no headset connected
            }
            if (p.size() < 7 || p[0] != bridge || p[1] != 0x10 || p[4] != 0x00) {
                return kWaiting; // our acknowledgement, events, other programs' traffic
            }
            if (p[5] == 0xFF && p[6] == feature) {
                return kFailed; // the headset's error
            }
            if (p[5] != feature || p[6] != tag) {
                return kWaiting;
            }
            reply.assign(p.begin() + 7, p.end());
            return kDone;
        });
    }

private:
    static constexpr uint8_t kRequestTag = 0x01; // software ID; not 0: the headset's own events use 0
    static constexpr int kWaiting = 0;
    static constexpr int kDone = 1;
    static constexpr int kFailed = 2;

    DWORD output;
    std::vector<uint8_t> prefix;
    Pipe pipe;

    bool write(const std::vector<uint8_t> &payload)
    {
        std::vector<uint8_t> frame(output, 0);
        if (prefix.size() + 2 + payload.size() > frame.size()) {
            return false;
        }
        std::copy(prefix.begin(), prefix.end(), frame.begin());
        frame[prefix.size()] = static_cast<uint8_t>(payload.size() + 1);
        frame[prefix.size() + 1] = 0x00; // flags: a single fragment
        std::copy(payload.begin(), payload.end(), frame.begin() + static_cast<std::ptrdiff_t>(prefix.size() + 2));
        return pipe.write(frame);
    }

    /** Reads frames until `check` says done or failed, or the time is up. */
    template <typename Check>
    bool await(Check check)
    {
        const ULONGLONG deadline = GetTickCount64() + 1000; // the headset answers over the air
        std::vector<uint8_t> r;
        while (!cancelled && pipe.reading) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline
                || WaitForSingleObject(pipe.overlapped.hEvent, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0) {
                return false;
            }
            if (!pipe.take(r) || r.size() < prefix.size() + 3 || !std::equal(prefix.begin(), prefix.end(), r.begin())) {
                continue;
            }
            size_t length = r[prefix.size()];
            if (length <= 1 || prefix.size() + 1 + length > r.size()) {
                continue;
            }
            std::vector<uint8_t> payload(r.begin() + static_cast<std::ptrdiff_t>(prefix.size() + 2),
                r.begin() + static_cast<std::ptrdiff_t>(prefix.size() + 1 + length));
            int result = check(payload);
            if (result != kWaiting) {
                return result == kDone;
            }
        }
        return false;
    }
};

/**
 * Asks a Centurion receiver for its headset's battery. False when the receiver doesn't answer;
 * `online` false when it does but no headset is connected to it.
 */
bool describeCenturion(Centurion &link, bool &online, int &percent, std::string &status)
{
    std::vector<uint8_t> r;
    // Root.getFeature(0x0003, the bridge) on the receiver
    if (!link.direct(0x00, 0x00, {0x00, 0x03}, r) || r.empty() || r[0] == 0) {
        return false;
    }
    link.bridge = r[0];
    // Root.getFeature(0x0104, battery state of charge) on the headset
    online = link.bridged(0x00, 0x00, {0x01, 0x04}, r) && !r.empty() && r[0] != 0;
    if (!online) {
        return true;
    }
    uint8_t index = r[0];
    // battery, function 0: <percent> <?> <state: 1 or 2 charging>
    if (link.bridged(index, 0x00, {}, r) && !r.empty() && r[0] <= 100) {
        percent = r[0];
        uint8_t state = r.size() >= 3 ? r[2] : 0;
        status = state == 1 || state == 2 ? "charging" : "discharging";
    }
    return true;
}

/** Devices without HID++ (or that didn't answer it): listed by the name Windows has for them. */
void listByName(int64_t id, const std::vector<const Interface *> &silent, const std::vector<std::string> &named)
{
    std::vector<std::string> sent = named;
    for (const Interface *iface : silent) {
        std::string name = clean(iface->product);
        bool seen = name.empty();
        for (const std::string &other : sent) {
            seen = seen || other == name;
        }
        if (!seen) {
            sent.push_back(name);
            send(id, "device", name + "\tdevice\tno HID++ (name only)\t-1\t");
        }
    }
}

std::string scan(int64_t id)
{
    std::vector<Interface> all = interfaces();
    std::vector<const Interface *> silent;
    std::vector<std::string> named; // products of interfaces that answered HID++
    for (const Interface &iface : all) {
        if (cancelled) {
            return "";
        }
        if (!iface.centurionPath.empty()) {
            Centurion link(iface);
            bool online = false;
            int percent = -1;
            std::string status;
            if (link.isOpen() && describeCenturion(link, online, percent, status)) {
                std::string name = clean(iface.product);
                std::string connection = online ? "Lightspeed receiver" : "receiver (headset off or out of range)";
                send(id, "device", name + "\theadset\t" + connection + "\t" + std::to_string(percent) + "\t" + status);
                named.push_back(name);
                continue;
            }
        }
        if (iface.longPath.empty()) {
            silent.push_back(&iface);
            continue;
        }
        Link link(iface);
        bool answered = false;
        // 0xFF is a device on its own cable; a receiver's devices sit in slots 1 to 6.
        for (int device : {0xFF, 1, 2, 3, 4, 5, 6}) {
            if (!link.isOpen()) {
                break;
            }
            if (cancelled) {
                return "";
            }
            std::string name;
            std::string kind = "device";
            if (!describe(link, static_cast<uint8_t>(device), name, kind)) {
                continue;
            }
            if (clean(name).empty()) {
                name = iface.product;
            }
            std::string connection = device == 0xFF
                ? "cable"
                : clean(iface.product) + ", slot " + std::to_string(device);
            int percent = -1;
            std::string status;
            battery(link, static_cast<uint8_t>(device), percent, status);
            const bool lighting = link.feature(static_cast<uint8_t>(device), kRgbEffects) != 0
                && link.feature(static_cast<uint8_t>(device), kPerKeyLighting) != 0;
            send(id, "device", clean(name) + "\t" + kind + "\t" + connection + "\t" + std::to_string(percent) + "\t" + status
                + "\t" + (lighting ? "1" : "0") + "\t" + utf8(iface.longPath.c_str()) + "|" + std::to_string(device));
            answered = true;
            named.push_back(clean(name));
            if (device == 0xFF) {
                break; // the interface is the device itself, not a receiver
            }
        }
        if (!answered && iface.otherVendor) {
            silent.push_back(&iface);
        }
    }
    if (cancelled) {
        return "";
    }
    listByName(id, silent, named);
    return "";
}

/**
 * Paints every key of a LIGHTSYNC keyboard one colour (0xRRGGBB), or hands its lighting back to
 * the keyboard (color < 0). The calls are Solaar's (settings_templates.py RGBControl and
 * PerKeyLighting, rgb_power.py): onboard profiles to host mode, RGB_EFFECTS SetSWControl, then
 * PER_KEY_LIGHTING_V2 range update and FrameEnd. Nothing is written to the keyboard's memory: a
 * replug, G HUB or giving it back restores the keyboard's own lighting. '' when it worked.
 */
std::string light(const std::string &address, int color)
{
    const size_t bar = address.rfind('|');
    if (bar == std::string::npos) {
        return "No device address";
    }
    const std::string path = address.substr(0, bar);
    const auto device = static_cast<uint8_t>(std::atoi(address.c_str() + bar + 1));
    for (const Interface &iface : interfaces()) {
        if (iface.longPath.empty() || utf8(iface.longPath.c_str()) != path) {
            continue;
        }
        Link link(iface);
        if (!link.isOpen()) {
            return "Couldn't open the keyboard";
        }
        uint8_t r[16];
        const uint8_t rgb = link.feature(device, kRgbEffects);
        const uint8_t perKey = rgb != 0 ? link.feature(device, kPerKeyLighting) : 0;
        if (rgb == 0 || perKey == 0) {
            return "The keyboard didn't answer (asleep or switched off?)";
        }
        const uint8_t onboard = link.feature(device, kOnboardProfiles);
        if (color < 0) {
            // SetSWControl(mode 0): the keyboard's own effects again; onboard profiles back on
            if (!link.request(device, rgb, 5, {0x01, 0x00, 0x00}, r)) {
                return "The keyboard refused to take its lighting back";
            }
            if (onboard != 0) {
                link.request(device, onboard, 1, {0x01}, r);
            }
            return "";
        }
        if (onboard != 0) {
            link.request(device, onboard, 1, {0x02}, r); // host mode: the profile's effects stop
        }
        // SetSWControl: set, mode 3 (software), flags 4 (the firmware keeps watching for idle)
        if (!link.request(device, rgb, 5, {0x01, 0x03, 0x04}, r)) {
            return "The keyboard didn't hand over its lighting";
        }
        // The key ids it has: a bitmap over three pages of getInfo (from byte 2), id 0 unused
        std::vector<uint8_t> bitmap;
        for (uint8_t page = 0; page < 3; page++) {
            if (link.request(device, perKey, 0, {0x00, 0x00, page}, r)) {
                bitmap.insert(bitmap.end(), r + 2, r + 16);
            }
        }
        int first = 0;
        int last = 0;
        for (int key = 1; key < 255 && key / 8 < static_cast<int>(bitmap.size()); key++) {
            if ((bitmap[key / 8] >> (key % 8)) & 1) {
                first = first == 0 ? key : first;
                last = key;
            }
        }
        if (first == 0) {
            return "The keyboard didn't list its keys";
        }
        const auto red = static_cast<uint8_t>(color >> 16);
        const auto green = static_cast<uint8_t>(color >> 8);
        const auto blue = static_cast<uint8_t>(color);
        if (!link.request(device, perKey, 5, {static_cast<uint8_t>(first), static_cast<uint8_t>(last), red, green, blue}, r)
            || !link.request(device, perKey, 7, {0x00}, r)) { // range update, then FrameEnd shows it
            return "The keyboard refused the colour";
        }
        return "";
    }
    return "The keyboard isn't connected any more; scan again";
}

} // namespace

#else

namespace {
std::string scan(int64_t)
{
    return "Only Windows is supported for now";
}

std::string light(const std::string &, int)
{
    return "Only Windows is supported for now";
}
} // namespace

#endif

namespace {

void stop()
{
    {
        std::lock_guard<std::mutex> lock(posting);
        cancelled = true;
    }
    std::lock_guard<std::mutex> lock(state);
    if (worker.joinable()) {
        worker.join();
    }
    running = false;
}

// The app normally resets modules when its event loop ends; if it exits some other way, the
// worker must still be joined before its std::thread is destroyed.
struct ShutdownGuard {
    ~ShutdownGuard() { stop(); }
} shutdownGuard;

/**
 * Runs one job (a scan, a lighting change) on the worker thread, which then posts "done" with the
 * job's error ('' when it worked). Returns the job's id; 0 while another job is still running,
 * since both talk to the same devices.
 */
int64_t start(std::function<std::string(int64_t)> job)
{
    std::lock_guard<std::mutex> lock(state);
    if (running) {
        return 0;
    }
    if (worker.joinable()) {
        worker.join();
    }
    cancelled = false;
    running = true;
    int64_t id = nextId++;
    worker = std::thread([id, job = std::move(job)] {
        std::string error = job(id);
        running = false;
        send(id, "done", error);
    });
    return id;
}

} // namespace

extern "C" {

RKM_API void rkm_logi_init(rk_post_fn fn)
{
    post = fn;
}

RKM_API void rkm_logi_reset(void)
{
    stop();
}

RKM_API int64_t rkm_logi_scan(void)
{
    return start([](int64_t id) { return scan(id); });
}

RKM_API int64_t rkm_logi_light(const char *address, size_t address_len, int64_t color)
{
    std::string target(address, address_len);
    return start([target, color](int64_t) { return light(target, static_cast<int>(color)); });
}

} // extern "C"
