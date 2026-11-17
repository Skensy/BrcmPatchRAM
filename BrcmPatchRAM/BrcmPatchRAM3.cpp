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
 *  BrcmPatchRAM3.cpp
 *  BrcmPatchRAM3
 *
 *  Created by Laura Müller on 06.10.19.
 *
 */

#if defined(TARGET_CATALINA)

// Silence warning about deprecated USB header files.
#define __IOUSBFAMILY__

#include <IOKit/usb/USB.h>
#include <IOKit/IOCatalogue.h>

#include <libkern/version.h>
#include <libkern/OSKextLib.h>

#include "Common.h"
#include "BrcmPatchRAM.h"

//////////////////////////////////////////////////////////////////////////////////////////////////

enum { kMyOffPowerState = 0, kMyOnPowerState = 1 };

static IOPMPowerState myTwoStates[2] =
{
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/*
 * Examining the log files I discovered that mPreResetDelay is obsolete
 * for the Dell DW1560 because the device implements some kind of
 * handshake mechanism to signal when it has finished processing
 * the downloaded firmware patches and is ready for the HCI reset
 * command by sending a Vendor Specific Event (event code 0xff).
 *
 * The corresponding log entry looks like this:
 *
 * BrcmPatchRAM2: [0a5c:216f]: Vendor specific event. Ready to reset device.
 *
 * hskSupport contains a list of all devices which have been verified to
 * support this mechanism. It must be terminated with a zero entry.
 *
 * I assume that more devices (all?) also work this way so that
 * they should be added to the list. It might be possible that this
 * is a common feature among Broadcom BT controllers making the list
 * obsolete, but for now, we still need it.
 */
static DeviceHskSupport hskSupport[] =
{
    { 0x0a5c, 0x216f },
    { 0x0a5c, 0x21ec },
    { 0x0a5c, 0x6412 },
    { 0x0a5c, 0x6414 },
    { 0x0489, 0xe07a },
    { 0x0,    0x0    }
};

OSDefineMetaClassAndStructors(BrcmPatchRAM3, IOService)

bool BrcmPatchRAM::init(OSDictionary *properties)
{
    bool result;
    
    DebugLog("init\n");

    result = super::init(properties);
    
    if (result) {
        UInt32 delay;
        
        mInitialDelay = 100;
        
        if (OSNumber* initialDelay = OSDynamicCast(OSNumber, getProperty("InitialDelay")))
            mInitialDelay = initialDelay->unsigned32BitValue();
        
        if (PE_parse_boot_argn("bpr_initialdelay", &delay, sizeof delay))
            mInitialDelay = delay;
        
        mPostResetDelay = 100;
        
        if (OSNumber* postResetDelay = OSDynamicCast(OSNumber, getProperty("PostResetDelay")))
            mPostResetDelay = postResetDelay->unsigned32BitValue();
        
        if (PE_parse_boot_argn("bpr_postresetdelay", &delay, sizeof delay))
            mPostResetDelay = delay;
        
        mPreResetDelay = 250;
        
        if (OSNumber* preResetDelay = OSDynamicCast(OSNumber, getProperty("PreResetDelay")))
            mPreResetDelay = preResetDelay->unsigned32BitValue();
        
        if (PE_parse_boot_argn("bpr_preresetdelay", &delay, sizeof delay))
            mPreResetDelay = delay;
    }
    return result;
}

void BrcmPatchRAM::free()
{
    DebugLog("free\n");
    
    OSSafeReleaseNULL(mFirmwareStore);
    super::free();
}

IOService* BrcmPatchRAM::probe(IOService *provider, SInt32 *probeScore)
{
    BrcmFirmwareStore *firmwareStore;
    OSString *firmwareKey;

    DebugLog("probe\n");
    
    AlwaysLog("Version %s starting on OS X Darwin %d.%d.\n", OSKextGetCurrentVersionString(), version_major, version_minor);
    
    /*
     * Preference towards starting BrcmPatchRAM3.kext when BrcmPatchRAM2.kext,
     * or BrcmPatchRAM.kext also exist.
     */
    *probeScore = 3000;
    
    // BrcmPatchRAM.kext, if installed on pre 10.11 fails immediately
    if (version_major < 15) {
        AlwaysLog("Aborting -- BrcmPatchRAM3.kext should not be installed pre 10.11.  Use BrcmPatchRAM.kext instead.\n");
        return NULL;
    }
    
    mDevice.setDevice(provider);
    
    if (!mDevice.getValidatedDevice()) {
        AlwaysLog("Provider type is incorrect (not IOUSBDevice or IOUSBHostDevice)\n");
        return NULL;
    }
    if (OSString* displayName = OSDynamicCast(OSString, getProperty(kDisplayName)))
        provider->setProperty(kUSBProductString, displayName);
    
    mVendorId = mDevice.getVendorID();
    mProductId = mDevice.getProductID();
    
    // Check if device supports handshake.
    int handshake;
    if (PE_parse_boot_argn("bpr_handshake", &handshake, sizeof handshake))
        mSupportsHandshake = handshake != 0;
    else
        mSupportsHandshake = supportsHandshake(mVendorId, mProductId);

    DebugLog("Device %s handshake.\n", mSupportsHandshake ? "supports" : "doesn't support");
    
    /* Get firmware for device. */
    firmwareKey = OSDynamicCast(OSString, getProperty(kFirmwareKey));
    
    if (firmwareKey) {
        firmwareStore = getFirmwareStore();
        
        if (firmwareStore)
            firmwareStore->getFirmware(mVendorId, mProductId, firmwareKey);
    }
    /* Release device again as probe() shouldn't alter it's state. */
    mDevice.setDevice(NULL);
    
    return super::probe(provider, probeScore);
}

bool BrcmPatchRAM::start(IOService *provider)
{
    uint64_t start_time, end_time, nano_secs;
    bool success = false;
    
    DebugLog("start\n");
    
    clock_get_uptime(&start_time);

    if (!super::start(provider))
        goto done;

    /*
     * Register for power state notifications as it seems to cause
     * a re-probe in case start fails to upload firmware due to
     * unexpected power state transitions. Without it, the driver
     * sometimes fails to upload firmware on boot but will succeed
     * later on wakeup.
     */
    PMinit();
    registerPowerDriver(this, myTwoStates, 2);
    provider->joinPMtree(this);
    makeUsable();
    
    mDevice.setDevice(provider);
    success = uploadFirmware();
    if (!success) {
        mDevice.setDevice(nullptr);
        PMstop();
        super::stop(provider);
    }

done:
    clock_get_uptime(&end_time);
    absolutetime_to_nanoseconds(end_time - start_time, &nano_secs);
    uint64_t milli_secs = nano_secs / 1000000;
    AlwaysLog("Processing time %llu.%llu seconds.\n", milli_secs / 1000, milli_secs % 1000);

    return success;
}

void BrcmPatchRAM::stop(IOService* provider)
{
    DebugLog("stop\n");
    
    PMstop();

    OSSafeReleaseNULL(mFirmwareStore);

    /* Release device. */
    mDevice.setDevice(NULL);
    
    super::stop(provider);
}

/*
 * As we registered for power state notifications we have to supply a
 * handler, even though it's a dummy implementation.
 */
IOReturn BrcmPatchRAM::setPowerState(unsigned long which, IOService *whom)
{
    DebugLog("setPowerState: which = 0x%lx\n", which);
    
    if (which == kMyOffPowerState) {
        
    } else if (which == kMyOnPowerState) {
        
    }
    return IOPMAckImplied;
}

bool BrcmPatchRAM::uploadFirmware()
{
    // Personalities without a FirmwareKey do not require an upload.
    if (!getProperty(kFirmwareKey))
        return true;
    mDevice.removeProperty(kFirmwareLoaded);
    if (!mDevice.open(this)) {
        AlwaysLog("uploadFirmware could not open the device!\n");
        return false;
    }
    bool success = false;
    // Print out additional device information
    printDeviceInfo();
    
    if (setConfiguration(0) && mDevice.openBluetoothInterface(this, &mInterface, &mInterruptPipe)) {
        success = performUpgrade();
        mInterruptPipe.setPipe(nullptr);
        mInterface.close(this);
    }
    if (success) {
        mDevice.setProperty(kFirmwareLoaded, true);
        AlwaysLog("[%04x:%04x]: Firmware %s.\n", mVendorId, mProductId,
                  mDeviceState == kUpdateComplete ? "upgrade completed successfully" : "upgrade not needed");
    } else {
        AlwaysLog("[%04x:%04x]: Firmware upgrade failed.\n", mVendorId, mProductId);
    }
    mInterface.setInterface(NULL);
    mDevice.close(this);
    return success;
}

BrcmFirmwareStore* BrcmPatchRAM::getFirmwareStore()
{
    if (!mFirmwareStore) {
        // check to see if it already loaded
        IOService* tmpStore = waitForClass(kBrcmFirmwareStoreService, 0);
        mFirmwareStore = OSDynamicCast(BrcmFirmwareStore, tmpStore);
        if (!mFirmwareStore)
        {
            // not loaded, so wait...
            if (tmpStore)
                tmpStore->release();
            tmpStore = waitForClass(kBrcmFirmwareStoreService, 2000UL*1000UL*1000UL);
            mFirmwareStore = OSDynamicCast(BrcmFirmwareStore, tmpStore);
            if (!mFirmwareStore && tmpStore)
                tmpStore->release();
        }
    }
    if (!mFirmwareStore)
        AlwaysLog("[%04x:%04x]: BrcmFirmwareStore does not appear to be available.\n", mVendorId, mProductId);
    
    return mFirmwareStore;
}

void BrcmPatchRAM::printDeviceInfo()
{
    char product[255];
    char manufacturer[255];
    char serial[255];
    
    // Retrieve device information
    mDevice.getStringDescriptor(mDevice.getProductStringIndex(), product, sizeof(product));
    mDevice.getStringDescriptor(mDevice.getManufacturerStringIndex(), manufacturer, sizeof(manufacturer));
    mDevice.getStringDescriptor(mDevice.getSerialNumberStringIndex(), serial, sizeof(serial));
    
    AlwaysLog("[%04x:%04x]: USB [%s v%d] \"%s\" by \"%s\"\n",
              mVendorId,
              mProductId,
              serial,
              mDevice.getDeviceRelease(),
              product,
              manufacturer);
}

bool BrcmPatchRAM::setConfiguration(int configurationIndex)
{
    IOReturn result;
    const USBCONFIGURATIONDESCRIPTOR* configurationDescriptor;
    UInt8 currentConfiguration = 0xFF;
    
    // Find the first config/interface
    UInt8 numconf = 0;
    
    if ((numconf = mDevice.getNumConfigurations()) < (configurationIndex + 1)) {
        AlwaysLog("[%04x:%04x]: Composite configuration index %d is not available, %d total composite configurations.\n",
                  mVendorId, mProductId, configurationIndex, numconf);
        return false;
    } else {
        DebugLog("[%04x:%04x]: Available composite configurations: %d.\n", mVendorId, mProductId, numconf);
    }
    configurationDescriptor = mDevice.getFullConfigurationDescriptor(configurationIndex);
    
    // Set the configuration to the requested configuration index
    if (!configurationDescriptor) {
        AlwaysLog("[%04x:%04x]: No configuration descriptor for configuration index: %d.\n", mVendorId, mProductId, configurationIndex);
        return false;
    }
    
    if ((result = mDevice.getConfiguration(this, &currentConfiguration)) != kIOReturnSuccess) {
        AlwaysLog("[%04x:%04x]: Unable to retrieve active configuration (\"%s\" 0x%08x).\n", mVendorId, mProductId, stringFromReturn(result), result);
        return false;
    }
    
    // Device is already configured
    if (currentConfiguration != 0) {
        DebugLog("[%04x:%04x]: Device configuration is already set to configuration index %d.\n",
                 mVendorId, mProductId, configurationIndex);
        return true;
    }
    
    // Set the configuration to the first configuration
    if ((result = mDevice.setConfiguration(this, configurationDescriptor->bConfigurationValue, true)) != kIOReturnSuccess) {
        AlwaysLog("[%04x:%04x]: Unable to (re-)configure device (\"%s\" 0x%08x).\n", mVendorId, mProductId, stringFromReturn(result), result);
        return false;
    }
    
    DebugLog("[%04x:%04x]: Set device configuration to configuration index %d successfully.\n",
             mVendorId, mProductId, configurationIndex);
    
    return true;
}

bool BrcmPatchRAM::supportsHandshake(UInt16 vid, UInt16 did)
{
    UInt32 i;
    
    for (i = 0; hskSupport[i].vid != 0; i++) {
        if ((hskSupport[i].vid == vid) && (hskSupport[i].did == did))
            return true;
    }
    return false;
}

#ifdef DEBUG
const char* BrcmPatchRAM::getState(DeviceState deviceState)
{
    static const IONamedValue state_values[] = {
        {kUnknown,            "Unknown"              },
        {kPreInitialize,      "PreInitialize"        },
        {kInitialize,         "Initialize"           },
        {kFirmwareVersion,    "Firmware version"     },
        {kMiniDriverComplete, "Mini-driver complete" },
        {kInstructionWrite,   "Instruction write"    },
        {kInstructionWritten, "Instruction written"  },
        {kFirmwareWritten,    "Firmware written"     },
        {kResetWrite,         "Reset write"          },
        {kResetComplete,      "Reset complete"       },
        {kUpdateComplete,     "Update complete"      },
        {kUpdateNotNeeded,    "Update not needed"    },
        {kUpdateAborted,      "Update aborted"       },
        {0,                   NULL                   }
    };
    
    return IOFindNameForValue(deviceState, state_values);
}
#endif //DEBUG

const char* BrcmPatchRAM::stringFromReturn(IOReturn rtn)
{
    static const IONamedValue IOReturn_values[] = {
        {kIOReturnIsoTooOld,          "Isochronous I/O request for distant past"     },
        {kIOReturnIsoTooNew,          "Isochronous I/O request for distant future"   },
        {kIOReturnNotFound,           "Data was not found"                           },
        //REVIEW: new error identifiers?
#ifndef TARGET_ELCAPITAN
        {kIOUSBUnknownPipeErr,        "Pipe ref not recognized"                      },
        {kIOUSBTooManyPipesErr,       "Too many pipes"                               },
        {kIOUSBNoAsyncPortErr,        "No async port"                                },
        {kIOUSBNotEnoughPowerErr,     "Not enough power for selected configuration"  },
        {kIOUSBEndpointNotFound,      "Endpoint not found"                           },
        {kIOUSBConfigNotFound,        "Configuration not found"                      },
        {kIOUSBTransactionTimeout,    "Transaction timed out"                        },
        {kIOUSBTransactionReturned,   "Transaction has been returned to the caller"  },
        {kIOUSBPipeStalled,           "Pipe has stalled, error needs to be cleared"  },
        {kIOUSBInterfaceNotFound,     "Interface reference not recognized"           },
        {kIOUSBLowLatencyBufferNotPreviouslyAllocated,
            "Attempted to user land low latency isoc calls w/out calling PrepareBuffer" },
        {kIOUSBLowLatencyFrameListNotPreviouslyAllocated,
            "Attempted to user land low latency isoc calls w/out calling PrepareBuffer" },
        {kIOUSBHighSpeedSplitError,   "Error on hi-speed bus doing split transaction"},
        {kIOUSBSyncRequestOnWLThread, "Synchronous USB request on workloop thread."  },
        {kIOUSBDeviceNotHighSpeed,    "The device is not a high speed device."       },
        {kIOUSBClearPipeStallNotRecursive,
            "IOUSBPipe::ClearPipeStall should not be called rescursively"               },
        {kIOUSBLinkErr,               "USB link error"                               },
        {kIOUSBNotSent2Err,           "Transaction not sent"                         },
        {kIOUSBNotSent1Err,           "Transaction not sent"                         },
        {kIOUSBNotEnoughPipesErr,     "Not enough pipes in interface"                },
        {kIOUSBBufferUnderrunErr,     "Buffer Underrun (Host hardware failure)"      },
        {kIOUSBBufferOverrunErr,      "Buffer Overrun (Host hardware failure"        },
        {kIOUSBReserved2Err,          "Reserved"                                     },
        {kIOUSBReserved1Err,          "Reserved"                                     },
        {kIOUSBWrongPIDErr,           "Pipe stall, Bad or wrong PID"                 },
        {kIOUSBPIDCheckErr,           "Pipe stall, PID CRC error"                    },
        {kIOUSBDataToggleErr,         "Pipe stall, Bad data toggle"                  },
        {kIOUSBBitstufErr,            "Pipe stall, bitstuffing"                      },
        {kIOUSBCRCErr,                "Pipe stall, bad CRC"                          },
#endif
        {0,                           NULL                                           }
    };
    
    const char* result = IOFindNameForValue(rtn, IOReturn_values);
    
    if (result)
        return result;
    
    return super::stringFromReturn(rtn);
}

#endif /* TARGET_CATALINA */

