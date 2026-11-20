/*
 *  Released under "The GNU General Public License (GPL-2.0)"
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *
 */

#if defined(TARGET_ELCAPITAN) || defined(TARGET_CATALINA)
#define __IOUSBFAMILY__
#include <IOKit/usb/USB.h>
#endif
#include "Common.h"
#include "BrcmPatchRAM.h"
#include "hci.h"

namespace {
struct InterruptRead {
    IOService* owner;
    IOLock* lock;
    IOBufferMemoryDescriptor* buffer;
    USBCOMPLETION completion;
    UInt16 length;
    IOReturn status;
    bool pending;
    bool detached;

    static InterruptRead* create(IOService* owner) {
        auto read = static_cast<InterruptRead*>(IOMalloc(sizeof(InterruptRead)));
        if (!read)
            return nullptr;
        *read = {};
        read->lock = IOLockAlloc();
        read->buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionIn, 0x200);
        if (!read->lock || !read->buffer || read->buffer->prepare(kIODirectionIn) != kIOReturnSuccess) {
            OSSafeReleaseNULL(read->buffer);
            if (read->lock)
                IOLockFree(read->lock);
            IOFree(read, sizeof(*read));
            return nullptr;
        }
        owner->retain();
        read->owner = owner;
        read->completion = { read, completed, nullptr };
        return read;
    }

    void destroy() {
        buffer->complete(kIODirectionIn);
        buffer->release();
        IOLockFree(lock);
        owner->release();
        IOFree(this, sizeof(*this));
    }

    static void completed(void* target, void*, IOReturn result, UInt32 count) {
        auto read = static_cast<InterruptRead*>(target);
        IOLockLock(read->lock);
        read->pending = false;
        bool detached = read->detached;
        if (!detached) {
            read->status = count <= read->buffer->getLength() ? result : kIOReturnOverrun;
#if defined(TARGET_ELCAPITAN) || defined(TARGET_CATALINA)
            read->length = static_cast<UInt16>(count);
#else
            read->length = static_cast<UInt16>(read->buffer->getLength() - count);
#endif
            IOLockWakeup(read->lock, read, true);
        }
        IOLockUnlock(read->lock);
        if (detached)
            read->destroy();
    }

    IOReturn receive(USBPipeShim& pipe, uint64_t deadline) {
        uint64_t now;
        clock_get_uptime(&now);
        if (now >= deadline)
            return kIOReturnTimeout;

        pending = true;
        IOReturn result = pipe.read(buffer, 0, 0, buffer->getLength(), &completion);
        if (result == kIOUSBPipeStalled) {
            result = pipe.clearStall();
            if (result == kIOReturnSuccess) {
                clock_get_uptime(&now);
                if (now >= deadline)
                    result = kIOReturnTimeout;
                else
                    result = pipe.read(buffer, 0, 0, buffer->getLength(), &completion);
            }
        }
        if (result != kIOReturnSuccess) {
            pending = false;
            return result;
        }

        IOLockLock(lock);
        while (pending) {
            int wait = IOLockSleepDeadline(lock, this, deadline, THREAD_UNINT);
            if (wait == THREAD_TIMED_OUT && pending) {
                IOLockUnlock(lock);
                result = pipe.abort();
                if (result != kIOReturnSuccess)
                    AlwaysLog("Interrupt pipe abort failed (0x%08x).\n", result);
                return kIOReturnTimeout;
            }
        }
        result = status;
        IOLockUnlock(lock);
        return result;
    }

    void finish() {
        IOLockLock(lock);
        // A queued completion owns the context after the uploader returns.
        bool releaseNow = !pending;
        detached = pending;
        IOLockUnlock(lock);
        if (releaseNow)
            destroy();
    }
};
}

IOReturn BrcmPatchRAM::writeInstruction(OSData* data)
{
    void* bytes = const_cast<void*>(data->getBytesNoCopy());
#ifdef TARGET_CATALINA
    return hciCommand(bytes, data->getLength());
#else
    mExpectedOpcode = OSReadLittleInt16(bytes, 0);
    auto buffer = IOMemoryDescriptor::withAddress(bytes, data->getLength(), kIODirectionOut);
    if (!buffer)
        return kIOReturnNoMemory;
    IOReturn result = buffer->prepare();
    if (result == kIOReturnSuccess) {
        result = mBulkPipe.write(buffer, 5000, 5000, buffer->getLength(), nullptr);
        IOReturn completed = buffer->complete();
        if (result == kIOReturnSuccess)
            result = completed;
    }
    buffer->release();
    return result;
#endif
}

IOService* BrcmPatchRAM::waitForClass(const char* name, uint64_t timeout)
{
    auto matching = serviceMatching(name);
    if (!matching)
        return nullptr;
    auto service = waitForMatchingService(matching, timeout);
    matching->release();
    return service;
}

IOReturn BrcmPatchRAM::hciCommand(void * command, UInt16 length)
{
    IOReturn result;

    mExpectedOpcode = OSReadLittleInt16(command, 0);
    if ((result = mInterface.hciCommand(command, length)) != kIOReturnSuccess)
        AlwaysLog("[%04x:%04x]: device request failed (\"%s\" 0x%08x).\n", mVendorId, mProductId, stringFromReturn(result), result);

    return result;
}

IOReturn BrcmPatchRAM::hciParseResponse(void* response, UInt16 length)
{
    HCI_RESPONSE* header = (HCI_RESPONSE*)response;
    IOReturn result = kIOReturnSuccess;

    if (length < sizeof(*header) || header->length > length - sizeof(*header)) {
        mDeviceState = kUpdateAborted;
        return kIOReturnBadArgument;
    }

    switch (header->eventCode) {
        case HCI_EVENT_COMMAND_COMPLETE:
        {
            HCI_COMMAND_COMPLETE* event = (HCI_COMMAND_COMPLETE*)response;

            constexpr size_t commandFields = sizeof(*event) - sizeof(*header) - sizeof(event->status);
            if (header->length < commandFields) {
                mDeviceState = kUpdateAborted;
                return kIOReturnError;
            }

            // Opcode zero advertises command credits and has no return parameters.
            if (!event->opcode || event->opcode != mExpectedOpcode)
                break;
            if (header->length < commandFields + sizeof(event->status)) {
                mDeviceState = kUpdateAborted;
                return kIOReturnBadArgument;
            }
            if (event->status != 0) {
                AlwaysLog("HCI command 0x%04x failed with status 0x%02x.\n", event->opcode, event->status);
                mDeviceState = kUpdateAborted;
                return kIOReturnError;
            }

            switch (event->opcode) {
                case HCI_OPCODE_READ_VERBOSE_CONFIG:
                    DebugLog("[%04x:%04x]: READ VERBOSE CONFIG complete (status: 0x%02x, length: %d bytes).\n",
                             mVendorId, mProductId, event->status, header->length);

                    if (header->length < 12 - sizeof(*header)) {
                        mDeviceState = kUpdateAborted;
                        return kIOReturnBadArgument;
                    }
                    mFirmwareVersion = OSReadLittleInt16(response, 10);

                    DebugLog("[%04x:%04x]: Firmware version: v%d.\n",
                             mVendorId, mProductId, mFirmwareVersion + 0x1000);

                    // Device does not require a firmware patch at this time
                    if (mFirmwareVersion > 0)
                        mDeviceState = kUpdateNotNeeded;
                    else
                        mDeviceState = kFirmwareVersion;
                    break;

                case HCI_OPCODE_DOWNLOAD_MINIDRIVER:
                    DebugLog("[%04x:%04x]: DOWNLOAD MINIDRIVER complete (status: 0x%02x, length: %d bytes).\n",
                             mVendorId, mProductId, event->status, header->length);

                    mDeviceState = kMiniDriverComplete;
                    break;

                case HCI_OPCODE_LAUNCH_RAM:
                    //DebugLog("[%04x:%04x]: LAUNCH RAM complete (status: 0x%02x, length: %d bytes).\n",
                    //          mVendorId, mProductId, event->status, header->length);

                    mDeviceState = kInstructionWritten;
                    break;

                case HCI_OPCODE_END_OF_RECORD:
                    DebugLog("[%04x:%04x]: END OF RECORD complete (status: 0x%02x, length: %d bytes).\n",
                             mVendorId, mProductId, event->status, header->length);

                    mDeviceState = kFirmwareWritten;
                    break;

                case HCI_OPCODE_RESET:
                    DebugLog("[%04x:%04x]: RESET complete (status: 0x%02x, length: %d bytes).\n",
                             mVendorId, mProductId, event->status, header->length);

                    mDeviceState = mDeviceState == kPreInitialize ? kInitialize : kResetComplete;
                    break;

                default:
                    DebugLog("[%04x:%04x]: Event COMMAND COMPLETE (opcode 0x%04x, status: 0x%02x, length: %d bytes).\n",
                             mVendorId, mProductId, event->opcode, event->status, header->length);
                    break;
            }

            break;
        }

        case HCI_EVENT_NUM_COMPLETED_PACKETS:
            DebugLog("[%04x:%04x]: Number of completed packets.\n", mVendorId, mProductId);
            break;

        case HCI_EVENT_CONN_COMPLETE:
            DebugLog("[%04x:%04x]: Connection complete event.\n", mVendorId, mProductId);
            break;

        case HCI_EVENT_DISCONN_COMPLETE:
            DebugLog("[%04x:%04x]: Disconnection complete. event\n", mVendorId, mProductId);
            break;

        case HCI_EVENT_HARDWARE_ERROR:
            mDeviceState = kUpdateAborted;
            if (header->length < 1)
                return kIOReturnBadArgument;
            AlwaysLog("[%04x:%04x]: HCI hardware error 0x%02x.\n", mVendorId, mProductId,
                      static_cast<const UInt8*>(response)[sizeof(*header)]);
            return kIOReturnError;

        case HCI_EVENT_MODE_CHANGE:
            DebugLog("[%04x:%04x]: Mode change event.\n", mVendorId, mProductId);
            break;

        case HCI_EVENT_LE_META:
            DebugLog("[%04x:%04x]: Low-Energy meta event.\n", mVendorId, mProductId);
            break;

        case HCI_EVENT_VENDOR:
            DebugLog("[%04x:%04x]: Vendor specific event. Ready to reset device.\n", mVendorId, mProductId);

            if (mSupportsHandshake && mDeviceState == kFirmwareWritten) {
                // Device is ready for reset.
                mDeviceState = kResetWrite;
            }
            break;

        default:
            DebugLog("[%04x:%04x]: Unknown event code (0x%02x).\n", mVendorId, mProductId, header->eventCode);
            break;
    }

    return result;
}

bool BrcmPatchRAM::performUpgrade()
{
    BrcmFirmwareStore* firmwareStore;
    OSArray* instructions = NULL;
    OSCollectionIterator* iterator = NULL;
    OSData* data;
#ifdef DEBUG
    DeviceState previousState = kUnknown;
#endif

    InterruptRead* read = InterruptRead::create(this);
    if (!read)
        return false;
    mDeviceState = kPreInitialize;

    while (true)
    {
#ifdef DEBUG
        if (mDeviceState != kInstructionWrite && mDeviceState != kInstructionWritten)
            DebugLog("[%04x:%04x]: State \"%s\" --> \"%s\".\n", mVendorId, mProductId, getState(previousState), getState(mDeviceState));

        previousState = mDeviceState;
#endif

        // Break out when done
        if (mDeviceState == kUpdateAborted || mDeviceState == kUpdateComplete || mDeviceState == kUpdateNotNeeded)
            break;

        // Note on following switch/case:
        //   use 'break' when a response from io completion callback is expected
        //   use 'continue' when a change of state with no expected response (loop again)

        switch (mDeviceState)
        {
            case kPreInitialize:
                /* Reset the device to put it in a defined state. */
                if (hciCommand(&HCI_RESET, sizeof(HCI_RESET)) != kIOReturnSuccess) {
                    DebugLog("HCI_RESET failed, aborting.");
                    mDeviceState = kUpdateAborted;
                    continue;
                }
                break;

            case kInitialize:
                /* Wait for device to become ready after reset. */
                IOSleep(mPostResetDelay);

                if (hciCommand(&HCI_VSC_READ_VERBOSE_CONFIG, sizeof(HCI_VSC_READ_VERBOSE_CONFIG)) != kIOReturnSuccess) {
                    DebugLog("HCI_VSC_READ_VERBOSE_CONFIG failed, aborting.");
                    mDeviceState = kUpdateAborted;
                    continue;
                }
                break;

            case kFirmwareVersion:
                // Unable to retrieve firmware store
                if (!(firmwareStore = getFirmwareStore())) {
                    mDeviceState = kUpdateAborted;
                    continue;
                }
                instructions = firmwareStore->getFirmware(mVendorId, mProductId, OSDynamicCast(OSString, getProperty(kFirmwareKey)));

                // Unable to retrieve firmware instructions
                if (!instructions) {
                    mDeviceState = kUpdateAborted;
                    continue;
                }

                // Initiate firmware upgrade
                if (hciCommand(&HCI_VSC_DOWNLOAD_MINIDRIVER, sizeof(HCI_VSC_DOWNLOAD_MINIDRIVER)) != kIOReturnSuccess) {
                    DebugLog("HCI_VSC_DOWNLOAD_MINIDRIVER failed, aborting.");
                    mDeviceState = kUpdateAborted;
                    continue;
                }
                break;

            case kMiniDriverComplete:
                // Should never happen, but semantically causes a leak.
                OSSafeReleaseNULL(iterator);
                // Write firmware data to bulk pipe
                iterator = OSCollectionIterator::withCollection(instructions);

                if (!iterator) {
                    mDeviceState = kUpdateAborted;
                    continue;
                }
                // If this IOSleep is not issued, the device is not ready to receive
                // the firmware instructions and we will deadlock due to lack of
                // responses.
                IOSleep(mInitialDelay);

                mDeviceState = kInstructionWrite;
                continue;

            case kInstructionWrite:
                // should never happen, but would cause a crash
                if (!iterator) {
                    mDeviceState = kUpdateAborted;
                    continue;
                }

                if ((data = OSDynamicCast(OSData, iterator->getNextObject()))) {
                    IOReturn result = writeInstruction(data);
                    if (result != kIOReturnSuccess) {
                        AlwaysLog("[%04x:%04x]: Firmware write failed (0x%08x).\n", mVendorId, mProductId, result);
                        mDeviceState = kUpdateAborted;
                        continue;
                    }
                } else {
                    // Firmware data fully written
                    if (hciCommand(&HCI_VSC_END_OF_RECORD, sizeof(HCI_VSC_END_OF_RECORD)) != kIOReturnSuccess) {
                        DebugLog("HCI_VSC_END_OF_RECORD failed, aborting.");
                        mDeviceState = kUpdateAborted;
                        continue;
                    }
                }
                break;

            case kInstructionWritten:
                mDeviceState = kInstructionWrite;
                continue;

            case kFirmwareWritten:
                if (!mSupportsHandshake) {
                    IOSleep(mPreResetDelay);

                    if (hciCommand(&HCI_RESET, sizeof(HCI_RESET)) != kIOReturnSuccess) {
                        DebugLog("HCI_RESET failed, aborting.");
                        mDeviceState = kUpdateAborted;
                        continue;
                    }
                }
                break;

            case kResetWrite:
                if (hciCommand(&HCI_RESET, sizeof(HCI_RESET)) != kIOReturnSuccess) {
                    DebugLog("HCI_RESET failed, aborting.");
                    mDeviceState = kUpdateAborted;
                    continue;
                }
                break;

            case kResetComplete: {
                IOSleep(mPostResetDelay);
                USBStatus status;
                IOReturn result = mDevice.getDeviceStatus(this, &status);
                if (result != kIOReturnSuccess)
                    AlwaysLog("[%04x:%04x]: Final USB status request failed (0x%08x).\n", mVendorId, mProductId, result);
                mDeviceState = result == kIOReturnSuccess ? kUpdateComplete : kUpdateAborted;
                continue;
            }

            case kUnknown:
            case kUpdateNotNeeded:
            case kUpdateComplete:
            case kUpdateAborted:
                DebugLog("Error: kUnkown/kUpdateComplete/kUpdateAborted cases should be unreachable.\n");
                break;
        }

        // Ignore unrelated events without resending a command or advancing the iterator.
        DeviceState waitingState = mDeviceState;
        if (waitingState == kFirmwareWritten && mSupportsHandshake)
            mExpectedOpcode = 0; // Only the vendor readiness event advances this state.
        uint64_t deadline;
        clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
        do {
            IOReturn result = read->receive(mInterruptPipe, deadline);
            if (result != kIOReturnSuccess) {
                AlwaysLog("[%04x:%04x]: Interrupt read failed (0x%08x).\n", mVendorId, mProductId, result);
                mDeviceState = kUpdateAborted;
                break;
            }
            hciParseResponse(read->buffer->getBytesNoCopy(), read->length);
        } while (mDeviceState == waitingState);
    }

    read->finish();
    OSSafeReleaseNULL(iterator);

    return mDeviceState == kUpdateComplete || mDeviceState == kUpdateNotNeeded;
}
