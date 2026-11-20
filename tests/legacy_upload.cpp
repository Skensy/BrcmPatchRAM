// Host doubles for the legacy IOService and kernel thread APIs.
#include <cassert>
#include <cstddef>
#define AlwaysLog(...) ((void)0)
#define DebugLog(...) ((void)0)
using thread_t = int;
using wait_result_t = int;
using IOReturn = int;
enum { kIOReturnSuccess, kFirmwareLoaded, kFirmwareKey, kUpdateComplete, kUSBInterrupt,
       kUSBIn, kUSBBulk, kUSBOut };
const int KERN_SUCCESS = 0;
static bool locked, failStart;
static int handles;
static void (*queued)(void*, int);
static void* target;
static int kernel_thread_start(void (*action)(void*, int), void* owner, thread_t* thread) {
    if (failStart) return 1;
    queued = action;
    target = owner;
    *thread = ++handles;
    return KERN_SUCCESS;
}
static void thread_deallocate(thread_t) { assert(handles == 1); --handles; }
static void IOLockLock(int) { assert(!locked); locked = true; }
static void IOLockUnlock(int) { assert(locked); locked = false; }
static int current_thread() { return 1; }
static void thread_terminate(int) { assert(!locked); }

struct Pipe {
    bool present = true;
    bool getValidatedPipe() { return present; }
    void setPipe(void*) { present = false; }
    void abort() {}
};
struct Interface {
    bool open(void*) { return true; }
    void close(void*) {}
    void setInterface(void*) {}
    void findPipe(Pipe*, int, int) {}
};
struct Device {
    bool loaded = false, present = true, opens = true;
    bool getValidatedDevice() { return present; }
    bool getProperty(int) { return loaded; }
    void setProperty(int, bool value) { loaded = value; }
    void removeProperty(int) { loaded = false; }
    bool open(void*) { assert(present); return opens; }
    void close(void*) {}
};
struct BrcmPatchRAM {
    Device mDevice;
    Interface mInterface;
    Pipe mBulkPipe, mInterruptPipe;
    int refs = 1, upgrades = 0, published = 0, mDeviceState = kUpdateComplete;
    static const int mLoadFirmwareLock = 0;
    unsigned generation = 1;
    unsigned powerGeneration() const { return generation; }
    bool key = true, upgradeOK = true, sleepDuringUpload = false;
    void retain() { ++refs; }
    void release() { assert(refs == 2); --refs; }
    bool getProperty(int) { return key; }
    void printDeviceInfo() {}
    bool setConfiguration(int) { return true; }
    bool findInterface(Interface*) { return true; }
    bool performUpgrade() { ++upgrades; if (sleepDuringUpload) ++generation; return upgradeOK; }
    void publishPersonality() { ++published; }
    IOReturn onTimerEvent();
    static void uploadFirmwareThread(void*, int);
    void uploadFirmware();
};
// DRIVER METHODS

int main() {
    for (int scenario = 0; scenario < 8; ++scenario) {
        BrcmPatchRAM driver;
        driver.key = scenario != 0;
        driver.mDevice.opens = scenario != 1;
        driver.upgradeOK = scenario != 2;
        driver.sleepDuringUpload = scenario == 7;
        failStart = scenario == 4;
        queued = nullptr;
        driver.onTimerEvent();
        assert(handles == 0);
        if (!failStart) {
            assert(queued && driver.refs == 2);
            driver.mDevice.present = scenario != 5; // stop completed before dispatch
            if (scenario == 6) ++driver.generation; // sleep before dispatch
            queued(target, 0);
        }
        assert(driver.refs == 1);
        assert(driver.mDevice.loaded == (scenario == 0 || scenario == 3));
        assert(driver.upgrades == (scenario == 2 || scenario == 3 || scenario == 7));
        assert(driver.published == (scenario == 0 || scenario == 3));
    }
    BrcmPatchRAM driver;
    driver.mDevice.loaded = true;
    driver.mDevice.opens = false;
    driver.uploadFirmware();
    assert(!driver.mDevice.loaded);
}
