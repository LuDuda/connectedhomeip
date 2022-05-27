/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "AppTask.h"

#include "AppConfig.h"
#include "AppEvent.h"
#include "LEDWidget.h"
#include "PWMDevice.h"
#include "ThreadUtil.h"

#include <DeviceInfoProviderImpl.h>
#include <app-common/zap-generated/attribute-id.h>
#include <app-common/zap-generated/attribute-type.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-id.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/server/Dnssd.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/ErrorStr.h>
#include <system/SystemClock.h>

#if CONFIG_CHIP_OTA_REQUESTOR
#include "OTAUtil.h"
#endif

#include <dk_buttons_and_leds.h>
#include <logging/log.h>
#include <zephyr.h>

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;

namespace {

constexpr int kFactoryResetTriggerTimeout      = 3000;
constexpr int kFactoryResetCancelWindowTimeout = 3000;
constexpr int kExtDiscoveryTimeoutSecs         = 20;
constexpr int kAppEventQueueSize               = 10;
constexpr uint8_t kButtonPushEvent             = 1;
constexpr uint8_t kButtonReleaseEvent          = 0;
constexpr EndpointId kLightEndpointId          = 1;
constexpr uint32_t kIdentifyBlinkRateMs        = 500;
constexpr uint8_t kDefaultMinLevel             = 0;
constexpr uint8_t kDefaultMaxLevel             = 254;

K_MSGQ_DEFINE(sAppEventQueue, sizeof(AppEvent), kAppEventQueueSize, alignof(AppEvent));
k_timer sFunctionTimer;

Identify sIdentify = { kLightEndpointId, AppTask::IdentifyStartHandler, AppTask::IdentifyStopHandler,
                       EMBER_ZCL_IDENTIFY_IDENTIFY_TYPE_VISIBLE_LED };

LEDWidget sStatusLED;
LEDWidget sIdentifyLED;
LEDWidget sUnusedLED;

bool sIsThreadProvisioned = false;
bool sIsThreadEnabled     = false;
bool sHaveBLEConnections  = false;

chip::DeviceLayer::DeviceInfoProviderImpl gExampleDeviceInfoProvider;

} // namespace


class MyDACProvider : public DeviceAttestationCredentialsProvider
{
public:
    CHIP_ERROR GetCertificationDeclaration(MutableByteSpan & out_cd_buffer) override
    {
        const uint8_t cd[] = {
            // here paste the output of `cat CD.der | xxd -i` command.
            0x30, 0x81, 0xe9, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01,
            0x07, 0x02, 0xa0, 0x81, 0xdb, 0x30, 0x81, 0xd8, 0x02, 0x01, 0x03, 0x31,
            0x0d, 0x30, 0x0b, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04,
            0x02, 0x01, 0x30, 0x44, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
            0x01, 0x07, 0x01, 0xa0, 0x37, 0x04, 0x35, 0x15, 0x24, 0x00, 0x01, 0x25,
            0x01, 0x68, 0x11, 0x36, 0x02, 0x05, 0x05, 0x80, 0x18, 0x24, 0x03, 0x0a,
            0x2c, 0x04, 0x13, 0x5a, 0x49, 0x47, 0x32, 0x30, 0x31, 0x34, 0x32, 0x5a,
            0x42, 0x33, 0x33, 0x30, 0x30, 0x30, 0x33, 0x2d, 0x32, 0x34, 0x24, 0x05,
            0x00, 0x24, 0x06, 0x00, 0x25, 0x07, 0x94, 0x26, 0x24, 0x08, 0x00, 0x18,
            0x31, 0x7e, 0x30, 0x7c, 0x02, 0x01, 0x03, 0x80, 0x14, 0x62, 0xfa, 0x82,
            0x33, 0x59, 0xac, 0xfa, 0xa9, 0x96, 0x3e, 0x1c, 0xfa, 0x14, 0x0a, 0xdd,
            0xf5, 0x04, 0xf3, 0x71, 0x60, 0x30, 0x0b, 0x06, 0x09, 0x60, 0x86, 0x48,
            0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86,
            0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x04, 0x48, 0x30, 0x46, 0x02, 0x21,
            0x00, 0xe3, 0x25, 0xc9, 0x28, 0x33, 0x82, 0xcf, 0xb7, 0x09, 0x7f, 0x72,
            0x83, 0x45, 0x4f, 0xb2, 0x9b, 0x20, 0xe8, 0xdc, 0x08, 0x8d, 0x74, 0x81,
            0x93, 0x25, 0x73, 0x52, 0xb3, 0x74, 0x98, 0x09, 0x79, 0x02, 0x21, 0x00,
            0xaf, 0x60, 0xaa, 0x15, 0x28, 0x65, 0xde, 0x47, 0x16, 0x6f, 0xb1, 0x35,
            0x74, 0xa2, 0x94, 0x24, 0x5b, 0x2b, 0xa4, 0x31, 0xd9, 0x50, 0x3c, 0xca,
            0x4b, 0x49, 0x23, 0x6d, 0xd5, 0x7b, 0x95, 0x68
        };

        return CopySpanToMutableSpan(ByteSpan(cd), out_cd_buffer);
    }

    CHIP_ERROR GetFirmwareInformation(MutableByteSpan &out_firmware_info_buffer) override
    {
        out_firmware_info_buffer.reduce_size(0);

        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetDeviceAttestationCert(MutableByteSpan & out_dac_buffer) override
    {
        const uint8_t dac[] = {
            // here paste the output of `cat DAC-Cert.der | xxd -i` command.
            0x30, 0x82, 0x01, 0xd6, 0x30, 0x82, 0x01, 0x7c, 0xa0, 0x03, 0x02, 0x01,
            0x02, 0x02, 0x08, 0x38, 0x3d, 0x1d, 0xcf, 0xf3, 0x3a, 0x38, 0x6a, 0x30,
            0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30,
            0x35, 0x31, 0x1d, 0x30, 0x1b, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x14,
            0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x4c, 0x65, 0x65, 0x64, 0x61,
            0x72, 0x73, 0x6f, 0x6e, 0x20, 0x50, 0x41, 0x49, 0x31, 0x14, 0x30, 0x12,
            0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x01,
            0x0c, 0x04, 0x31, 0x31, 0x36, 0x38, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x32,
            0x30, 0x35, 0x32, 0x36, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x17,
            0x0d, 0x34, 0x39, 0x31, 0x30, 0x31, 0x30, 0x32, 0x33, 0x35, 0x39, 0x35,
            0x39, 0x5a, 0x30, 0x4b, 0x31, 0x1d, 0x30, 0x1b, 0x06, 0x03, 0x55, 0x04,
            0x03, 0x0c, 0x14, 0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x4c, 0x65,
            0x65, 0x64, 0x61, 0x72, 0x73, 0x6f, 0x6e, 0x20, 0x44, 0x41, 0x43, 0x31,
            0x14, 0x30, 0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xa2,
            0x7c, 0x02, 0x01, 0x0c, 0x04, 0x31, 0x31, 0x36, 0x38, 0x31, 0x14, 0x30,
            0x12, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02,
            0x02, 0x0c, 0x04, 0x38, 0x30, 0x30, 0x35, 0x30, 0x59, 0x30, 0x13, 0x06,
            0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86,
            0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0xb9, 0xbb,
            0xce, 0x8c, 0x49, 0x65, 0xb6, 0xae, 0x3d, 0xcf, 0xc6, 0x91, 0x1f, 0x2d,
            0xa5, 0xac, 0x51, 0xcd, 0xc1, 0x07, 0x9a, 0xe7, 0xad, 0x02, 0x46, 0xc5,
            0x37, 0xc0, 0x07, 0x7b, 0x03, 0x40, 0xb1, 0x27, 0x71, 0xa2, 0x7b, 0x6d,
            0xf6, 0xcf, 0x21, 0x9b, 0x87, 0x0b, 0x4c, 0xbe, 0x8e, 0x63, 0xc2, 0xba,
            0x09, 0x27, 0xc4, 0x7a, 0x3b, 0x82, 0x77, 0x8f, 0xb3, 0xc0, 0x42, 0x51,
            0x5a, 0x6b, 0xa3, 0x60, 0x30, 0x5e, 0x30, 0x0c, 0x06, 0x03, 0x55, 0x1d,
            0x13, 0x01, 0x01, 0xff, 0x04, 0x02, 0x30, 0x00, 0x30, 0x0e, 0x06, 0x03,
            0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04, 0x04, 0x03, 0x02, 0x07, 0x80,
            0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0x54,
            0xbb, 0x74, 0xf0, 0x34, 0x69, 0xfe, 0x07, 0x7c, 0x78, 0xa5, 0x04, 0x0d,
            0xef, 0x95, 0xcf, 0x6f, 0x36, 0x95, 0x93, 0x30, 0x1f, 0x06, 0x03, 0x55,
            0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xfc, 0xd1, 0x7c, 0xf4,
            0xb4, 0x5a, 0xd1, 0x96, 0x35, 0x6f, 0x06, 0x04, 0xf1, 0x2c, 0x0b, 0xb7,
            0x21, 0xc6, 0x42, 0xff, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce,
            0x3d, 0x04, 0x03, 0x02, 0x03, 0x48, 0x00, 0x30, 0x45, 0x02, 0x20, 0x24,
            0xbf, 0x68, 0x87, 0xd1, 0x41, 0xbd, 0x5b, 0x4e, 0x35, 0x69, 0x9f, 0xac,
            0xb7, 0x22, 0x50, 0xb0, 0xb5, 0x7f, 0x8c, 0xc8, 0xe5, 0x36, 0x82, 0x62,
            0x44, 0x31, 0x80, 0xa9, 0x53, 0x14, 0x91, 0x02, 0x21, 0x00, 0xbf, 0x90,
            0x97, 0x50, 0x4d, 0xe8, 0x2f, 0xca, 0xfb, 0x84, 0xd8, 0x45, 0x16, 0xeb,
            0x23, 0x69, 0x4c, 0x3e, 0xd9, 0xd7, 0x6d, 0x43, 0x3f, 0x51, 0x42, 0x02,
            0x64, 0x9c, 0xf0, 0x62, 0x78, 0x13
        };

        return CopySpanToMutableSpan(ByteSpan(dac), out_dac_buffer);
    }

    CHIP_ERROR GetProductAttestationIntermediateCert(MutableByteSpan &out_pai_buffer) override
    {
        const uint8_t pai[] = {
            // here paste the output of `cat PAI-Cert.der | xxd -i` command.
            0x30, 0x82, 0x01, 0xac, 0x30, 0x82, 0x01, 0x51, 0xa0, 0x03, 0x02, 0x01,
            0x02, 0x02, 0x08, 0x33, 0x64, 0x95, 0x5a, 0x4a, 0x78, 0xf5, 0x65, 0x30,
            0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x30,
            0x1a, 0x31, 0x18, 0x30, 0x16, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x0f,
            0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x54, 0x65, 0x73, 0x74, 0x20,
            0x50, 0x41, 0x41, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x32, 0x30, 0x35, 0x32,
            0x36, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x5a, 0x17, 0x0d, 0x34, 0x39,
            0x31, 0x30, 0x31, 0x30, 0x32, 0x33, 0x35, 0x39, 0x35, 0x39, 0x5a, 0x30,
            0x35, 0x31, 0x1d, 0x30, 0x1b, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x14,
            0x4d, 0x61, 0x74, 0x74, 0x65, 0x72, 0x20, 0x4c, 0x65, 0x65, 0x64, 0x61,
            0x72, 0x73, 0x6f, 0x6e, 0x20, 0x50, 0x41, 0x49, 0x31, 0x14, 0x30, 0x12,
            0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xa2, 0x7c, 0x02, 0x01,
            0x0c, 0x04, 0x31, 0x31, 0x36, 0x38, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07,
            0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
            0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x57, 0x7b, 0x3d,
            0x59, 0x90, 0x73, 0x45, 0x8f, 0xec, 0x4c, 0xcc, 0x42, 0x83, 0x2a, 0x38,
            0x3e, 0x6f, 0x51, 0x02, 0x94, 0x3a, 0x9f, 0x48, 0xa1, 0xd9, 0x4b, 0xa0,
            0x87, 0x3b, 0x83, 0x77, 0xef, 0x99, 0xcd, 0x9e, 0x52, 0xe6, 0x43, 0x02,
            0x29, 0x3c, 0x70, 0x9f, 0x54, 0x6c, 0x18, 0xd0, 0x2a, 0xfa, 0xb6, 0xf4,
            0xae, 0x9f, 0xe2, 0xc0, 0xff, 0x94, 0xe6, 0xa2, 0xcc, 0x7d, 0x5f, 0x99,
            0x4c, 0xa3, 0x66, 0x30, 0x64, 0x30, 0x12, 0x06, 0x03, 0x55, 0x1d, 0x13,
            0x01, 0x01, 0xff, 0x04, 0x08, 0x30, 0x06, 0x01, 0x01, 0xff, 0x02, 0x01,
            0x01, 0x30, 0x0e, 0x06, 0x03, 0x55, 0x1d, 0x0f, 0x01, 0x01, 0xff, 0x04,
            0x04, 0x03, 0x02, 0x01, 0x06, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e,
            0x04, 0x16, 0x04, 0x14, 0xfc, 0xd1, 0x7c, 0xf4, 0xb4, 0x5a, 0xd1, 0x96,
            0x35, 0x6f, 0x06, 0x04, 0xf1, 0x2c, 0x0b, 0xb7, 0x21, 0xc6, 0x42, 0xff,
            0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80,
            0x14, 0x78, 0x5c, 0xe7, 0x05, 0xb8, 0x6b, 0x8f, 0x4e, 0x6f, 0xc7, 0x93,
            0xaa, 0x60, 0xcb, 0x43, 0xea, 0x69, 0x68, 0x82, 0xd5, 0x30, 0x0a, 0x06,
            0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x49, 0x00,
            0x30, 0x46, 0x02, 0x21, 0x00, 0xe9, 0xea, 0x13, 0x2c, 0x6d, 0x61, 0xe9,
            0x9b, 0x08, 0x50, 0x4e, 0xec, 0xab, 0x21, 0x16, 0xaa, 0x51, 0xd0, 0xa2,
            0x91, 0x53, 0xdf, 0x56, 0xd9, 0xfd, 0x2b, 0x69, 0x38, 0x7b, 0xed, 0xee,
            0xb1, 0x02, 0x21, 0x00, 0x85, 0xeb, 0x0c, 0xc6, 0xc1, 0x7e, 0x32, 0x63,
            0xcf, 0xe2, 0xb9, 0xa7, 0x08, 0x41, 0x47, 0x7a, 0x74, 0x51, 0xe9, 0x58,
            0xed, 0xa5, 0x38, 0xb4, 0xb8, 0x18, 0xbe, 0x7d, 0x18, 0xa2, 0xfc, 0x55
        };

        return CopySpanToMutableSpan(ByteSpan(pai), out_pai_buffer);
    }

    CHIP_ERROR LoadKeypairFromRaw(ByteSpan private_key, ByteSpan public_key, Crypto::P256Keypair & keypair)
    {
        Crypto::P256SerializedKeypair serialized_keypair;

        ReturnErrorOnFailure(serialized_keypair.SetLength(private_key.size() + public_key.size()));
        memcpy(serialized_keypair.Bytes(), public_key.data(), public_key.size());
        memcpy(serialized_keypair.Bytes() + public_key.size(), private_key.data(), private_key.size());

        return keypair.Deserialize(serialized_keypair);
    }

    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan &digest_to_sign, MutableByteSpan & out_signature_buffer) override
    {
        const uint8_t dacPriv[] = {
            // here paste the output of:
            // openssl ec -text -noout -in DAC-Key.pem 2>/dev/null | sed '0,/priv:$/d' | head -n3 | sed 's/:/ /g' | sed 's/\</0x/g' | sed 's/\>/,/g' | sed "s/^[ \t]*/   /" | sed 's/ *$//'
            0xc0, 0xd9, 0x49, 0x77, 0x6c, 0x21, 0xb4, 0x73, 0xed, 0xfb, 0x4f, 0x5e, 0x03, 0x9f, 0xc7,
            0x36, 0x8c, 0xda, 0xc2, 0x49, 0x9b, 0x41, 0x9d, 0xdf, 0x09, 0xba, 0x4f, 0x12, 0x1d, 0x6d,
            0x26, 0xaa,
        };

        const uint8_t dacPub[] = {
            // here paste the output of:
            // openssl ec -text -noout -in DAC-Key.pem 2>/dev/null | sed '0,/pub:$/d' | head -n5 | sed 's/:/ /g' | sed 's/\</0x/g' | sed 's/\>/,/g' | sed "s/^[ \t]*/   /" | sed 's/ *$//'

            0x04, 0xb9, 0xbb, 0xce, 0x8c, 0x49, 0x65, 0xb6, 0xae, 0x3d, 0xcf, 0xc6, 0x91, 0x1f, 0x2d,
            0xa5, 0xac, 0x51, 0xcd, 0xc1, 0x07, 0x9a, 0xe7, 0xad, 0x02, 0x46, 0xc5, 0x37, 0xc0, 0x07,
            0x7b, 0x03, 0x40, 0xb1, 0x27, 0x71, 0xa2, 0x7b, 0x6d, 0xf6, 0xcf, 0x21, 0x9b, 0x87, 0x0b,
            0x4c, 0xbe, 0x8e, 0x63, 0xc2, 0xba, 0x09, 0x27, 0xc4, 0x7a, 0x3b, 0x82, 0x77, 0x8f, 0xb3,
            0xc0, 0x42, 0x51, 0x5a, 0x6b,
        };

        Crypto::P256ECDSASignature signature;
        Crypto::P256Keypair keypair;

        ReturnErrorOnFailure(LoadKeypairFromRaw(ByteSpan(dacPriv), ByteSpan(dacPub), keypair));
        ReturnErrorOnFailure(keypair.ECDSA_sign_hash(digest_to_sign.data(), digest_to_sign.size(), signature));

        return CopySpanToMutableSpan(ByteSpan{ signature.ConstBytes(), signature.Length() }, out_signature_buffer);
   }
};

AppTask AppTask::sAppTask;

CHIP_ERROR AppTask::Init()
{
    // Initialize CHIP stack
    LOG_INF("Init CHIP stack");

    CHIP_ERROR err = chip::Platform::MemoryInit();
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("Platform::MemoryInit() failed");
        return err;
    }

    err = PlatformMgr().InitChipStack();
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("PlatformMgr().InitChipStack() failed");
        return err;
    }

    err = ThreadStackMgr().InitThreadStack();
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("ThreadStackMgr().InitThreadStack() failed");
        return err;
    }

#ifdef CONFIG_OPENTHREAD_MTD
    err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_MinimalEndDevice);
#else
    err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_Router);
#endif
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("ConnectivityMgr().SetThreadDeviceType() failed");
        return err;
    }

    // Initialize LEDs
    LEDWidget::InitGpio();
    LEDWidget::SetStateUpdateCallback(LEDStateUpdateHandler);

    sStatusLED.Init(SYSTEM_STATE_LED);
    sIdentifyLED.Init(DK_LED3);
    sUnusedLED.Init(DK_LED4);

    UpdateStatusLED();

    // Initialize lighting device (PWM)
    uint8_t minLightLevel = kDefaultMinLevel;
    Clusters::LevelControl::Attributes::MinLevel::Get(kLightEndpointId, &minLightLevel);

    uint8_t maxLightLevel = kDefaultMaxLevel;
    Clusters::LevelControl::Attributes::MaxLevel::Get(kLightEndpointId, &maxLightLevel);

    int ret = mPWMDevice.Init(LIGHTING_PWM_DEVICE, LIGHTING_PWM_CHANNEL, minLightLevel, maxLightLevel, maxLightLevel);
    if (ret != 0)
    {
        return chip::System::MapErrorZephyr(ret);
    }
    mPWMDevice.SetCallbacks(ActionInitiated, ActionCompleted);

    // Initialize buttons
    ret = dk_buttons_init(ButtonEventHandler);
    if (ret)
    {
        LOG_ERR("dk_buttons_init() failed");
        return chip::System::MapErrorZephyr(ret);
    }

    // Initialize function button timer
    k_timer_init(&sFunctionTimer, &AppTask::TimerEventHandler, nullptr);
    k_timer_user_data_set(&sFunctionTimer, this);

#ifdef CONFIG_MCUMGR_SMP_BT
    // Initialize DFU over SMP
    GetDFUOverSMP().Init(RequestSMPAdvertisingStart);
#ifndef CONFIG_CHIP_OTA_REQUESTOR
    // When OTA Requestor is enabled, it is responsible for confirming new images.
    GetDFUOverSMP().ConfirmNewImage();
#endif
#endif

    // Initialize CHIP server
    static MyDACProvider dacProvider;
    SetDeviceAttestationCredentialsProvider(&dacProvider);

    chip::app::DnssdServer::Instance().SetExtendedDiscoveryTimeoutSecs(kExtDiscoveryTimeoutSecs);

    static chip::CommonCaseDeviceServerInitParams initParams;
    (void) initParams.InitializeStaticResourcesBeforeServerInit();
    ReturnErrorOnFailure(chip::Server::GetInstance().Init(initParams));

    gExampleDeviceInfoProvider.SetStorageDelegate(&Server::GetInstance().GetPersistentStorage());
    chip::DeviceLayer::SetDeviceInfoProvider(&gExampleDeviceInfoProvider);

#if CONFIG_CHIP_OTA_REQUESTOR
    InitBasicOTARequestor();
#endif
    ConfigurationMgr().LogDeviceConfig();
    PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

    // Add CHIP event handler and start CHIP thread.
    // Note that all the initialization code should happen prior to this point to avoid data races
    // between the main and the CHIP threads.
    PlatformMgr().AddEventHandler(ChipEventHandler, 0);

    err = PlatformMgr().StartEventLoopTask();
    if (err != CHIP_NO_ERROR)
    {
        LOG_ERR("PlatformMgr().StartEventLoopTask() failed");
    }

    return err;
}

CHIP_ERROR AppTask::StartApp()
{
    ReturnErrorOnFailure(Init());

    AppEvent event = {};

    while (true)
    {
        k_msgq_get(&sAppEventQueue, &event, K_FOREVER);
        DispatchEvent(&event);
    }

    return CHIP_NO_ERROR;
}

void AppTask::LightingActionEventHandler(AppEvent * aEvent)
{
    PWMDevice::Action_t action = PWMDevice::INVALID_ACTION;
    int32_t actor              = 0;

    if (aEvent->Type == AppEvent::kEventType_Lighting)
    {
        action = static_cast<PWMDevice::Action_t>(aEvent->LightingEvent.Action);
        actor  = aEvent->LightingEvent.Actor;
    }
    else if (aEvent->Type == AppEvent::kEventType_Button)
    {
        action = GetAppTask().mPWMDevice.IsTurnedOn() ? PWMDevice::OFF_ACTION : PWMDevice::ON_ACTION;
        actor  = AppEvent::kEventType_Button;
    }

    if (action != PWMDevice::INVALID_ACTION && GetAppTask().mPWMDevice.InitiateAction(action, actor, NULL))
        LOG_INF("Action is already in progress or active.");
}

void AppTask::ButtonEventHandler(uint32_t button_state, uint32_t has_changed)
{
    AppEvent button_event;
    button_event.Type = AppEvent::kEventType_Button;

    if (LIGHTING_BUTTON_MASK & button_state & has_changed)
    {
        button_event.ButtonEvent.PinNo  = LIGHTING_BUTTON;
        button_event.ButtonEvent.Action = kButtonPushEvent;
        button_event.Handler            = LightingActionEventHandler;
        sAppTask.PostEvent(&button_event);
    }

    if (FUNCTION_BUTTON_MASK & has_changed)
    {
        button_event.ButtonEvent.PinNo  = FUNCTION_BUTTON;
        button_event.ButtonEvent.Action = (FUNCTION_BUTTON_MASK & button_state) ? kButtonPushEvent : kButtonReleaseEvent;
        button_event.Handler            = FunctionHandler;
        sAppTask.PostEvent(&button_event);
    }

    if (THREAD_START_BUTTON_MASK & button_state & has_changed)
    {
        button_event.ButtonEvent.PinNo  = THREAD_START_BUTTON;
        button_event.ButtonEvent.Action = kButtonPushEvent;
        button_event.Handler            = StartThreadHandler;
        sAppTask.PostEvent(&button_event);
    }

    if (BLE_ADVERTISEMENT_START_BUTTON_MASK & button_state & has_changed)
    {
        button_event.ButtonEvent.PinNo  = BLE_ADVERTISEMENT_START_BUTTON;
        button_event.ButtonEvent.Action = kButtonPushEvent;
        button_event.Handler            = StartBLEAdvertisementHandler;
        sAppTask.PostEvent(&button_event);
    }
}

void AppTask::TimerEventHandler(k_timer * timer)
{
    AppEvent event;
    event.Type               = AppEvent::kEventType_Timer;
    event.TimerEvent.Context = k_timer_user_data_get(timer);
    event.Handler            = FunctionTimerEventHandler;
    sAppTask.PostEvent(&event);
}

void AppTask::IdentifyStartHandler(Identify *)
{
    AppEvent event;
    event.Type    = AppEvent::kEventType_IdentifyStart;
    event.Handler = [](AppEvent *) { sIdentifyLED.Blink(kIdentifyBlinkRateMs); };
    sAppTask.PostEvent(&event);
}

void AppTask::IdentifyStopHandler(Identify *)
{
    AppEvent event;
    event.Type    = AppEvent::kEventType_IdentifyStop;
    event.Handler = [](AppEvent *) { sIdentifyLED.Set(false); };
    sAppTask.PostEvent(&event);
}

void AppTask::FunctionTimerEventHandler(AppEvent * aEvent)
{
    if (aEvent->Type != AppEvent::kEventType_Timer)
        return;

    // If we reached here, the button was held past kFactoryResetTriggerTimeout, initiate factory reset
    if (sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_SoftwareUpdate)
    {
        LOG_INF("Factory Reset Triggered. Release button within %ums to cancel.", kFactoryResetTriggerTimeout);

        // Start timer for kFactoryResetCancelWindowTimeout to allow user to cancel, if required.
        sAppTask.StartTimer(kFactoryResetCancelWindowTimeout);
        sAppTask.mFunction = kFunction_FactoryReset;

        // Turn off all LEDs before starting blink to make sure blink is co-ordinated.
        sStatusLED.Set(false);
        sIdentifyLED.Set(false);
        sUnusedLED.Set(false);

        sStatusLED.Blink(500);
        sIdentifyLED.Blink(500);
        sUnusedLED.Blink(500);
    }
    else if (sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_FactoryReset)
    {
        // Actually trigger Factory Reset
        sAppTask.mFunction = kFunction_NoneSelected;

        chip::Server::GetInstance().ScheduleFactoryReset();
    }
}

#ifdef CONFIG_MCUMGR_SMP_BT
void AppTask::RequestSMPAdvertisingStart(void)
{
    AppEvent event;
    event.Type    = AppEvent::kEventType_StartSMPAdvertising;
    event.Handler = [](AppEvent *) { GetDFUOverSMP().StartBLEAdvertising(); };
    sAppTask.PostEvent(&event);
}
#endif

void AppTask::FunctionHandler(AppEvent * aEvent)
{
    if (aEvent->ButtonEvent.PinNo != FUNCTION_BUTTON)
        return;

    // To trigger software update: press the FUNCTION_BUTTON button briefly (< kFactoryResetTriggerTimeout)
    // To initiate factory reset: press the FUNCTION_BUTTON for kFactoryResetTriggerTimeout + kFactoryResetCancelWindowTimeout
    // All LEDs start blinking after kFactoryResetTriggerTimeout to signal factory reset has been initiated.
    // To cancel factory reset: release the FUNCTION_BUTTON once all LEDs start blinking within the
    // kFactoryResetCancelWindowTimeout
    if (aEvent->ButtonEvent.Action == kButtonPushEvent)
    {
        if (!sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_NoneSelected)
        {
            sAppTask.StartTimer(kFactoryResetTriggerTimeout);

            sAppTask.mFunction = kFunction_SoftwareUpdate;
        }
    }
    else
    {
        // If the button was released before factory reset got initiated, trigger a software update.
        if (sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_SoftwareUpdate)
        {
            sAppTask.CancelTimer();
            sAppTask.mFunction = kFunction_NoneSelected;

#ifdef CONFIG_MCUMGR_SMP_BT
            GetDFUOverSMP().StartServer();
#else
            LOG_INF("Software update is disabled");
#endif
        }
        else if (sAppTask.mFunctionTimerActive && sAppTask.mFunction == kFunction_FactoryReset)
        {
            sIdentifyLED.Set(false);
            sUnusedLED.Set(false);
            UpdateStatusLED();
            sAppTask.CancelTimer();
            sAppTask.mFunction = kFunction_NoneSelected;
            LOG_INF("Factory Reset has been Canceled");
        }
    }
}

void AppTask::StartThreadHandler(AppEvent * aEvent)
{
    if (aEvent->ButtonEvent.PinNo != THREAD_START_BUTTON)
        return;

    if (!chip::DeviceLayer::ConnectivityMgr().IsThreadProvisioned())
    {
        StartDefaultThreadNetwork();
        LOG_INF("Device is not commissioned to a Thread network. Starting with the default configuration.");
    }
    else
    {
        LOG_INF("Device is commissioned to a Thread network.");
    }
}

void AppTask::StartBLEAdvertisementHandler(AppEvent *)
{
    if (Server::GetInstance().GetFabricTable().FabricCount() != 0)
    {
        LOG_INF("Matter service BLE advertising not started - device is already commissioned");
        return;
    }

    if (ConnectivityMgr().IsBLEAdvertisingEnabled())
    {
        LOG_INF("BLE advertising is already enabled");
        return;
    }

    if (Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow() != CHIP_NO_ERROR)
    {
        LOG_ERR("OpenBasicCommissioningWindow() failed");
    }
}

void AppTask::UpdateLedStateEventHandler(AppEvent * aEvent)
{
    if (aEvent->Type == AppEvent::kEventType_UpdateLedState)
    {
        aEvent->UpdateLedStateEvent.LedWidget->UpdateState();
    }
}

void AppTask::LEDStateUpdateHandler(LEDWidget & ledWidget)
{
    AppEvent event;
    event.Type                          = AppEvent::kEventType_UpdateLedState;
    event.Handler                       = UpdateLedStateEventHandler;
    event.UpdateLedStateEvent.LedWidget = &ledWidget;
    sAppTask.PostEvent(&event);
}

void AppTask::UpdateStatusLED()
{
    /* Update the status LED.
     *
     * If thread and service provisioned, keep the LED On constantly.
     *
     * If the system has ble connection(s) uptill the stage above, THEN blink the LED at an even
     * rate of 100ms.
     *
     * Otherwise, blink the LED On for a very short time. */
    if (sIsThreadProvisioned && sIsThreadEnabled)
    {
        sStatusLED.Set(true);
    }
    else if (sHaveBLEConnections)
    {
        sStatusLED.Blink(100, 100);
    }
    else
    {
        sStatusLED.Blink(50, 950);
    }
}

void AppTask::ChipEventHandler(const ChipDeviceEvent * event, intptr_t /* arg */)
{
    switch (event->Type)
    {
    case DeviceEventType::kCHIPoBLEAdvertisingChange:
#ifdef CONFIG_CHIP_NFC_COMMISSIONING
        if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Started)
        {
            if (NFCMgr().IsTagEmulationStarted())
            {
                LOG_INF("NFC Tag emulation is already started");
            }
            else
            {
                ShareQRCodeOverNFC(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
            }
        }
        else if (event->CHIPoBLEAdvertisingChange.Result == kActivity_Stopped)
        {
            NFCMgr().StopTagEmulation();
        }
#endif
        sHaveBLEConnections = ConnectivityMgr().NumBLEConnections() != 0;
        UpdateStatusLED();
        break;
    case DeviceEventType::kThreadStateChange:
        sIsThreadProvisioned = ConnectivityMgr().IsThreadProvisioned();
        sIsThreadEnabled     = ConnectivityMgr().IsThreadEnabled();
        UpdateStatusLED();
        break;
    default:
        break;
    }
}

void AppTask::CancelTimer()
{
    k_timer_stop(&sFunctionTimer);
    mFunctionTimerActive = false;
}

void AppTask::StartTimer(uint32_t aTimeoutInMs)
{
    k_timer_start(&sFunctionTimer, K_MSEC(aTimeoutInMs), K_NO_WAIT);
    mFunctionTimerActive = true;
}

void AppTask::ActionInitiated(PWMDevice::Action_t aAction, int32_t aActor)
{
    if (aAction == PWMDevice::ON_ACTION)
    {
        LOG_INF("Turn On Action has been initiated");
    }
    else if (aAction == PWMDevice::OFF_ACTION)
    {
        LOG_INF("Turn Off Action has been initiated");
    }
    else if (aAction == PWMDevice::LEVEL_ACTION)
    {
        LOG_INF("Level Action has been initiated");
    }
}

void AppTask::ActionCompleted(PWMDevice::Action_t aAction, int32_t aActor)
{
    if (aAction == PWMDevice::ON_ACTION)
    {
        LOG_INF("Turn On Action has been completed");
    }
    else if (aAction == PWMDevice::OFF_ACTION)
    {
        LOG_INF("Turn Off Action has been completed");
    }
    else if (aAction == PWMDevice::LEVEL_ACTION)
    {
        LOG_INF("Level Action has been completed");
    }

    if (aActor == AppEvent::kEventType_Button)
    {
        sAppTask.UpdateClusterState();
    }
}

void AppTask::PostLightingActionRequest(PWMDevice::Action_t aAction)
{
    AppEvent event;
    event.Type                 = AppEvent::kEventType_Lighting;
    event.LightingEvent.Action = aAction;
    event.Handler              = LightingActionEventHandler;
    PostEvent(&event);
}

void AppTask::PostEvent(AppEvent * aEvent)
{
    if (k_msgq_put(&sAppEventQueue, aEvent, K_NO_WAIT) != 0)
    {
        LOG_INF("Failed to post event to app task event queue");
    }
}

void AppTask::DispatchEvent(AppEvent * aEvent)
{
    if (aEvent->Handler)
    {
        aEvent->Handler(aEvent);
    }
    else
    {
        LOG_INF("Event received with no handler. Dropping event.");
    }
}

void AppTask::UpdateClusterState()
{
    // write the new on/off value
    EmberAfStatus status = Clusters::OnOff::Attributes::OnOff::Set(kLightEndpointId, mPWMDevice.IsTurnedOn());

    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        LOG_ERR("Updating on/off cluster failed: %x", status);
    }

    status = Clusters::LevelControl::Attributes::CurrentLevel::Set(kLightEndpointId, mPWMDevice.GetLevel());

    if (status != EMBER_ZCL_STATUS_SUCCESS)
    {
        LOG_ERR("Updating level cluster failed: %x", status);
    }
}
