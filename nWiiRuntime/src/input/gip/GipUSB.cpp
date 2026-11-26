// Ported from PowerEngine/Axmol; see UPSTREAM.json and LICENSE.
#include "GipGamepad.h"

#if NWII_HAVE_GIP
#include <libusb.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

namespace nwii::gip
{
namespace
{
using Clock = std::chrono::steady_clock;

std::string location(libusb_device* usb, const libusb_device_descriptor& descriptor)
{
    std::string result = std::to_string(descriptor.idVendor) + ":" + std::to_string(descriptor.idProduct) + ":" +
                         std::to_string(libusb_get_bus_number(usb));
    uint8_t ports[8];
    int count = libusb_get_port_numbers(usb, ports, 8);
    if (count > 0)
        for (int i = 0; i < count; ++i) result += "." + std::to_string(ports[i]);
    else result += ":address-" + std::to_string(libusb_get_device_address(usb));
    return result;
}

struct Queue
{
    std::mutex mutex;
    std::deque<Event> events;
    void push(Event event)
    {
        std::lock_guard<std::mutex> guard(mutex);
        // Coalesce axes, but preserve short button/Guide transitions between
        // render frames. Bound memory when a paused app stops draining input.
        if (!events.empty())
        {
            auto& last = events.back();
            if (last.device.path == event.device.path && last.connected == event.connected &&
                last.state.buttons == event.state.buttons && last.state.guide == event.state.guide &&
                (last.state.leftTrigger >= 256) == (event.state.leftTrigger >= 256) &&
                (last.state.rightTrigger >= 256) == (event.state.rightTrigger >= 256))
            {
                last = std::move(event);
                return;
            }
        }
        if (events.size() == 1024) events.pop_front();
        events.push_back(std::move(event));
    }
};

struct Connection
{
    libusb_device_handle* handle = nullptr;
    uint8_t input = 0, output = 0, inputType = 0, outputType = 0;
    int capacity = 64;
    bool claimed = false;
    int error = 0;
    ~Connection()
    {
        if (claimed) libusb_release_interface(handle, 0);
        if (handle) libusb_close(handle);
    }

    bool open(libusb_device* device)
    {
        libusb_config_descriptor* config = nullptr;
        if (libusb_get_active_config_descriptor(device, &config) != 0 &&
            libusb_get_config_descriptor(device, 0, &config) != 0) return false;
        int alternate = -1;
        for (int i = 0; i < config->bNumInterfaces; ++i)
            for (int j = 0; j < config->interface[i].num_altsetting; ++j)
            {
                const auto& port = config->interface[i].altsetting[j];
                if (port.bInterfaceNumber != 0 || port.bInterfaceClass != 0xFF || alternate >= 0) continue;
                for (int e = 0; e < port.bNumEndpoints; ++e)
                {
                    const auto& endpoint = port.endpoint[e];
                    const int type = endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                    if (type != LIBUSB_TRANSFER_TYPE_INTERRUPT && type != LIBUSB_TRANSFER_TYPE_BULK) continue;
                    if (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN)
                    {
                        input = endpoint.bEndpointAddress;
                        inputType = type;
                        capacity = std::max(64, int(endpoint.wMaxPacketSize));
                    }
                    else { output = endpoint.bEndpointAddress; outputType = type; }
                }
                if (input && output) alternate = port.bAlternateSetting;
                else input = output = 0;
            }
        const int configuration = config->bConfigurationValue;
        libusb_free_config_descriptor(config);
        if (alternate < 0) return false;
        error = libusb_open(device, &handle);
        if (error != 0) return false;
        // Leave an OS-owned gamepad on its native backend. In particular, do
        // not detach Linux xpad or claim a HID interface from GameController.
        if (libusb_kernel_driver_active(handle, 0) == 1) return false;
        int current = 0;
        if (libusb_get_configuration(handle, &current) != 0) return false;
        if (current != configuration && (error = libusb_set_configuration(handle, configuration)) != 0) return false;
        error = libusb_claim_interface(handle, 0);
        if (error != 0) return false;
        claimed = true;
        return alternate == 0 || libusb_set_interface_alt_setting(handle, 0, alternate) == 0;
    }

    int transfer(uint8_t endpoint, uint8_t type, uint8_t* bytes, int size, int& transferred, unsigned timeout)
    {
        return type == LIBUSB_TRANSFER_TYPE_INTERRUPT
            ? libusb_interrupt_transfer(handle, endpoint, bytes, size, &transferred, timeout)
            : libusb_bulk_transfer(handle, endpoint, bytes, size, &transferred, timeout);
    }
    bool write(const uint8_t* bytes, size_t size)
    {
        int count = 0;
        return transfer(output, outputType, const_cast<uint8_t*>(bytes), int(size), count, 100) == 0 && count == int(size);
    }
    int read(std::vector<uint8_t>& bytes, unsigned timeout)
    {
        bytes.resize(capacity);
        int count = 0;
        int error = transfer(input, inputType, bytes.data(), capacity, count, timeout);
        bytes.resize(std::max(0, count));
        return error == 0 && count <= 0 ? LIBUSB_ERROR_IO : error;
    }
};

class Reader
{
public:
    libusb_device* usb;
    Reader(libusb_device* device, Device info, Queue& queue) : usb(libusb_ref_device(device)), info_(std::move(info)), queue_(queue)
    {
        worker_ = std::thread([this] { run(); });
    }
    ~Reader()
    {
        requestStop();
        if (worker_.joinable()) worker_.join();
        libusb_unref_device(usb);
    }
    void requestStop() { stopping_ = true; wake_.notify_all(); }

private:
    Device info_;
    Queue& queue_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::mutex waitMutex_;
    std::condition_variable wake_;
    State state_;
    bool connected_ = false;
    bool inputSeen_ = false;
    uint8_t sequence_ = 0;
    int lastOpenError_ = 0;

    bool pause(int milliseconds)
    {
        std::unique_lock<std::mutex> lock(waitMutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return stopping_.load(); });
        return !stopping_;
    }
    bool acknowledge(Connection& connection, const std::vector<uint8_t>& bytes)
    {
        std::array<uint8_t, 13> reply;
        if (!acknowledgement(bytes.data(), bytes.size(), uint8_t(sequence_ + 1), reply)) return true;
        if (!connection.write(reply.data(), reply.size())) return false;
        ++sequence_;
        return true;
    }
    bool drainSetup(Connection& connection, unsigned timeout)
    {
        std::vector<uint8_t> bytes;
        for (int i = 0; i < 30 && !stopping_; ++i)
        {
            int error = connection.read(bytes, timeout);
            if (error == LIBUSB_ERROR_TIMEOUT) return true;
            if (error || !acknowledge(connection, bytes)) return false;
            if (decode(bytes.data(), bytes.size(), state_) == Packet::Input) inputSeen_ = true;
        }
        return !stopping_;
    }
    bool initialize(Connection& connection)
    {
        state_ = {};
        inputSeen_ = false;
        sequence_ = 0;
        std::vector<uint8_t> bytes;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!stopping_ && Clock::now() < deadline)
        {
            int error = connection.read(bytes, 100);
            if (error == LIBUSB_ERROR_TIMEOUT) continue;
            if (error || !acknowledge(connection, bytes)) return false;
            if (decode(bytes.data(), bytes.size(), state_) == Packet::Input) inputSeen_ = true;
            if (bytes.size() >= 4 && bytes[0] == 0x02) break;
        }
        const uint8_t power[] = {0x05, 0x20, 0, 0x01, 0};
        // New firmware uses this shorter extended-power packet despite the
        // header length; preserve the wire format used by both references.
        const uint8_t extended[] = {0x05, 0x20, 0, 0x0F, 0x06, 0, 0, 0, 0, 0, 0, 0x55, 0x53};
        if (!pause(20) || !connection.write(power, sizeof(power)) || !pause(50) || !drainSetup(connection, 80) ||
            !connection.write(extended, sizeof(extended)) || !pause(50) || !drainSetup(connection, 80)) return false;
        if (info_.vendor == 0x0E6F)
        {
            const uint8_t led[] = {0x0A, 0x20, 0, 0x03, 0, 1, 0x14};
            const uint8_t auth1[] = {0x06, 0x20, 0, 0x02, 1, 0};
            const uint8_t auth2[] = {0x06, 0x20, 0, 0x02, 2, 0};
            if (!connection.write(led, sizeof(led)) || !pause(30) || !connection.write(auth1, sizeof(auth1)) ||
                !pause(30) || !connection.write(auth2, sizeof(auth2)) || !pause(30)) return false;
        }
        if (!drainSetup(connection, 50)) return false;
        if (info_.vendor == 0x24C6)
        {
            // Supported PowerA pads need a brief motor enable before accepting later rumble commands.
            auto kick = rumblePacket(19006, 19006, ++sequence_);
            if (!connection.write(kick.data(), kick.size()) || !pause(10)) return false;
        }
        auto neutral = rumblePacket(0, 0, ++sequence_);
        return connection.write(neutral.data(), neutral.size());
    }
    void disconnect()
    {
        info_.output->connected = false;
        info_.output->rumble = 0;
        state_ = {};
        if (connected_) queue_.push({info_, state_, false});
        connected_ = false;
    }
    void run()
    {
        while (!stopping_)
        {
            {
                Connection connection;
                if (connection.open(usb) && initialize(connection))
                {
                    lastOpenError_ = 0;
                    info_.output->rumble = 0;
                    info_.output->connected = true;
                    connected_ = true;
                    queue_.push({info_, state_, true, inputSeen_});
                    int failures = 0;
                    uint64_t lastRumble = 0;
                    auto lastMotorWrite = Clock::now();
                    std::vector<uint8_t> bytes;
                    while (!stopping_)
                    {
                        const uint64_t requested = info_.output->rumble.load();
                        const auto elapsed = Clock::now() - lastMotorWrite;
                        if (elapsed >= std::chrono::milliseconds(10) &&
                            (requested != lastRumble || (uint32_t(requested) && elapsed >= std::chrono::seconds(2))))
                        {
                            auto motor = rumblePacket(uint16_t(requested), uint16_t(requested >> 16), ++sequence_);
                            if (!connection.write(motor.data(), motor.size())) break;
                            lastRumble = requested;
                            lastMotorWrite = Clock::now();
                        }
                        int error = connection.read(bytes, 8);
                        if (error == LIBUSB_ERROR_TIMEOUT) { failures = 0; continue; }
                        if (error)
                        {
                            if ((error == LIBUSB_ERROR_IO || error == LIBUSB_ERROR_INTERRUPTED ||
                                 error == LIBUSB_ERROR_OVERFLOW || error == LIBUSB_ERROR_PIPE) && ++failures <= 8)
                            {
                                if (error == LIBUSB_ERROR_PIPE) libusb_clear_halt(connection.handle, connection.input);
                                if (!pause(8)) break;
                                continue;
                            }
                            break;
                        }
                        failures = 0;
                        if (!acknowledge(connection, bytes)) break;
                        Packet kind = decode(bytes.data(), bytes.size(), state_);
                        if (bytes.size() >= 4 && bytes[0] == 0x02) break;  // Device restarted: initialize again.
                        if (kind == Packet::Input || (connected_ && kind == Packet::Guide))
                        {
                            connected_ = true;
                            queue_.push({info_, state_, true, true});
                        }
                    }
                    auto neutral = rumblePacket(0, 0, ++sequence_);
                    connection.write(neutral.data(), neutral.size());
                }
                else if (connection.error && connection.error != lastOpenError_)
                {
                    lastOpenError_ = connection.error;
                    std::fprintf(stderr, "nwii::gip: cannot open %s (%s): %s\n", info_.name.c_str(), info_.path.c_str(),
                                 libusb_error_name(connection.error));
                }
                disconnect();
            }
            if (!pause(250)) break;
        }
        disconnect();
    }
};
}

struct USBHub::Impl
{
    Queue queue;
    std::atomic<bool> stopping{false};
    std::thread discovery;
    std::mutex waitMutex;
    std::condition_variable wake;

    void run()
    {
        libusb_context* context = nullptr;
        const int error = libusb_init(&context);
        if (error != 0)
        {
            std::fprintf(stderr, "nwii::gip: libusb initialization failed: %s\n", libusb_error_name(error));
            return;
        }
        std::map<std::string, std::unique_ptr<Reader>> readers;
        while (!stopping)
        {
            libusb_device** devices = nullptr;
            ssize_t count = libusb_get_device_list(context, &devices);
            if (count >= 0)
            {
                std::map<std::string, libusb_device*> present;
                for (ssize_t i = 0; i < count; ++i)
                {
                    libusb_device_descriptor descriptor;
                    if (libusb_get_device_descriptor(devices[i], &descriptor) != 0) continue;
                    const char* name = productName(descriptor.idVendor, descriptor.idProduct);
                    if (!name) continue;
                    Device info{location(devices[i], descriptor), name, descriptor.idVendor, descriptor.idProduct};
                    present[info.path] = devices[i];
                    auto found = readers.find(info.path);
                    if (found != readers.end() && found->second->usb != devices[i]) readers.erase(found);
                    if (!readers.count(info.path) && readers.size() < 16)
                        readers.emplace(info.path, std::make_unique<Reader>(devices[i], info, queue));
                }
                for (auto it = readers.begin(); it != readers.end();)
                    if (!present.count(it->first)) it = readers.erase(it); else ++it;
                libusb_free_device_list(devices, 1);
            }
            std::unique_lock<std::mutex> lock(waitMutex);
            wake.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping.load(); });
        }
        for (auto& [path, reader] : readers) reader->requestStop();
        readers.clear();
        libusb_exit(context);
    }
};

USBHub::USBHub() = default;
USBHub::~USBHub() { stop(); }
void USBHub::start()
{
    if (impl_) return;
    auto impl = std::make_unique<Impl>();
    impl->discovery = std::thread([state = impl.get()] { state->run(); });
    impl_ = std::move(impl);
}
void USBHub::stop()
{
    if (!impl_) return;
    impl_->stopping = true;
    impl_->wake.notify_all();
    if (impl_->discovery.joinable()) impl_->discovery.join();
    impl_.reset();
}
std::vector<Event> USBHub::drain()
{
    if (!impl_) return {};
    std::lock_guard<std::mutex> guard(impl_->queue.mutex);
    std::vector<Event> events(std::make_move_iterator(impl_->queue.events.begin()), std::make_move_iterator(impl_->queue.events.end()));
    impl_->queue.events.clear();
    return events;
}
bool USBHub::available() { return true; }
}  // namespace nwii::gip

#else
namespace nwii::gip
{
struct USBHub::Impl {};
USBHub::USBHub() = default;
USBHub::~USBHub() = default;
void USBHub::start() {}
void USBHub::stop() {}
std::vector<Event> USBHub::drain() { return {}; }
bool USBHub::available() { return false; }
}
#endif
