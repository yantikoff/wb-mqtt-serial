#include "serial_config.h"
#include "file_utils.h"
#include "log.h"

#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

#include "tcp_port.h"
#include "tcp_port_settings.h"

#include "serial_port.h"
#include "serial_port_settings.h"

#include "config_merge_template.h"
#include "config_schema_generator.h"
#include "file_utils.h"

#include "devices/curtains/dooya_device.h"
#include "devices/curtains/somfy_sdn_device.h"
#include "devices/curtains/windeco_device.h"
#include "devices/dlms_device.h"
#include "devices/energomera_iec_device.h"
#include "devices/energomera_iec_mode_c_device.h"
#include "devices/ivtm_device.h"
#include "devices/lls_device.h"
#include "devices/mercury200_device.h"
#include "devices/mercury230_device.h"
#include "devices/milur_device.h"
#include "devices/modbus_device.h"
#include "devices/modbus_io_device.h"
#include "devices/neva_device.h"
#include "devices/pulsar_device.h"
#include "devices/s2k_device.h"
#include "devices/uniel_device.h"

#define LOG(logger) ::logger.Log() << "[serial config] "

using namespace std;
using namespace WBMQTT::JSON;

namespace
{
    const char* DefaultProtocol = "modbus";

    bool EndsWith(const string& str, const string& suffix)
    {
        return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    template<class T> T Read(const Json::Value& root, const std::string& key, const T& defaultValue)
    {
        T value;
        if (Get(root, key, value)) {
            return value;
        }
        return defaultValue;
    }

    int GetIntFromString(const std::string& value, const std::string& errorPrefix)
    {
        try {
            return std::stoi(value, /*pos= */ 0, /*base= */ 0);
        } catch (const std::logic_error&) {
            throw TConfigParserException(errorPrefix + ": plain integer or '0x..' hex string expected instead of '" +
                                         value + "'");
        }
    }

    int ToInt(const Json::Value& v, const std::string& title)
    {
        if (v.isInt())
            return v.asInt();
        if (!v.isString()) {
            throw TConfigParserException(title + ": plain integer or '0x..' hex string expected");
        }
        return GetIntFromString(v.asString(), title);
    }

    double ToDouble(const Json::Value& v, const std::string& title)
    {
        if (v.isNumeric())
            return v.asDouble();
        if (!v.isString()) {
            throw TConfigParserException(title + ": number or '0x..' hex string expected");
        }
        return GetIntFromString(v.asString(), title);
    }

    uint64_t ToUint64(const Json::Value& v, const string& title)
    {
        if (v.isUInt())
            return v.asUInt64();
        if (v.isInt()) {
            int val = v.asInt64();
            if (val >= 0) {
                return val;
            }
        }

        if (v.isString()) {
            auto val = v.asString();
            if (val.find("-") == std::string::npos) {
                // don't try to parse strings containing munus sign
                try {
                    return stoull(val, /*pos= */ 0, /*base= */ 0);
                } catch (const logic_error& e) {
                }
            }
        }

        throw TConfigParserException(
            title + ": 32 bit plain unsigned integer (64 bit when quoted) or '0x..' hex string expected instead of '" +
            v.asString() + "'");
    }

    int GetInt(const Json::Value& obj, const std::string& key)
    {
        return ToInt(obj[key], key);
    }

    double GetDouble(const Json::Value& obj, const std::string& key)
    {
        return ToDouble(obj[key], key);
    }

    bool ReadChannelsReadonlyProperty(const Json::Value& register_data,
                                      const std::string& key,
                                      bool templateReadonly,
                                      const std::string& override_error_message_prefix,
                                      const std::string& register_type)
    {
        if (!register_data.isMember(key)) {
            return templateReadonly;
        }
        auto& val = register_data[key];
        if (!val.isConvertibleTo(Json::booleanValue)) {
            return templateReadonly;
        }
        bool readonly = val.asBool();
        if (templateReadonly && !readonly) {
            LOG(Warn) << override_error_message_prefix << " unable to make register of type \"" << register_type
                      << "\" writable";
            return true;
        }
        return readonly;
    }

    const TRegisterType& GetRegisterType(const Json::Value& itemData, const TDeviceConfig& deviceConfig)
    {
        if (itemData.isMember("reg_type")) {
            std::string type = itemData["reg_type"].asString();
            try {
                return deviceConfig.TypeMap->Find(type);
            } catch (...) {
                throw TConfigParserException("invalid setup register type: " + type + " -- " + deviceConfig.DeviceType);
            }
        }
        return deviceConfig.TypeMap->GetDefaultType();
    }

    std::optional<std::chrono::milliseconds> GetReadRateLimit(const Json::Value& data)
    {
        std::chrono::milliseconds res(-1);
        try {
            Get(data, "poll_interval", res);
        } catch (...) { // poll_interval is deprecated, so ignore it, if it has wrong format
        }
        Get(data, "read_rate_limit_ms", res);
        if (res < 0ms) {
            return std::nullopt;
        }
        return std::make_optional(res);
    }

    std::optional<std::chrono::milliseconds> GetReadPeriod(const Json::Value& data)
    {
        std::chrono::milliseconds res(-1);
        Get(data, "read_period_ms", res);
        if (res < 0ms) {
            return std::nullopt;
        }
        return std::make_optional(res);
    }

    struct TLoadingContext
    {
        // Full path to loaded item composed from device and channels names
        std::string name_prefix;

        // MQTT topic prefix. It could be different from name_prefix
        std::string mqtt_prefix;
        const std::string& device_template_title;
        const IDeviceFactory& factory;
        const IRegisterAddress& device_base_address;
        size_t stride = 0;
        TTitleTranslations translated_name_prefixes;
        const Json::Value* translations = nullptr;

        TLoadingContext(const std::string& template_title,
                        const IDeviceFactory& f,
                        const IRegisterAddress& base_address)
            : device_template_title(template_title),
              factory(f),
              device_base_address(base_address)
        {}
    };

    struct TLoadRegisterConfigResult
    {
        PRegisterConfig RegisterConfig;
        std::string DefaultControlType;
    };

    TLoadRegisterConfigResult LoadRegisterConfig(const Json::Value& register_data,
                                                 const TDeviceConfig& device_config,
                                                 const std::string& readonly_override_error_message_prefix,
                                                 const TLoadingContext& context)
    {
        TLoadRegisterConfigResult res;
        TRegisterType regType = GetRegisterType(register_data, device_config);
        res.DefaultControlType = regType.DefaultControlType.empty() ? "text" : regType.DefaultControlType;

        if (register_data.isMember("format")) {
            regType.DefaultFormat = RegisterFormatFromName(register_data["format"].asString());
        }

        if (register_data.isMember("word_order")) {
            regType.DefaultWordOrder = WordOrderFromName(register_data["word_order"].asString());
        }

        double scale = Read(register_data, "scale", 1.0); // TBD: check for zero, too
        double offset = Read(register_data, "offset", 0.0);
        double round_to = Read(register_data, "round_to", 0.0);

        bool readonly = ReadChannelsReadonlyProperty(register_data,
                                                     "readonly",
                                                     regType.ReadOnly,
                                                     readonly_override_error_message_prefix,
                                                     regType.Name);
        // For compatibility with old configs
        readonly = ReadChannelsReadonlyProperty(register_data,
                                                "channel_readonly",
                                                readonly,
                                                readonly_override_error_message_prefix,
                                                regType.Name);

        auto registerDesc = context.factory.GetRegisterAddressFactory().LoadRegisterAddress(
            register_data,
            context.device_base_address,
            context.stride,
            RegisterFormatByteWidth(regType.DefaultFormat));

        if ((regType.DefaultFormat == RegisterFormat::String) && (registerDesc.DataWidth == 0)) {
            throw TConfigParserException(readonly_override_error_message_prefix +
                                         ": String size is not set for register string format");
        }

        res.RegisterConfig = TRegisterConfig::Create(regType.Index,
                                                     registerDesc,
                                                     regType.DefaultFormat,
                                                     scale,
                                                     offset,
                                                     round_to,
                                                     readonly,
                                                     regType.Name,
                                                     regType.DefaultWordOrder);

        if (register_data.isMember("error_value")) {
            res.RegisterConfig->ErrorValue = TRegisterValue{ToUint64(register_data["error_value"], "error_value")};
        }

        if (register_data.isMember("unsupported_value")) {
            res.RegisterConfig->UnsupportedValue =
                TRegisterValue{ToUint64(register_data["unsupported_value"], "unsupported_value")};
        }

        res.RegisterConfig->ReadRateLimit = GetReadRateLimit(register_data);
        res.RegisterConfig->ReadPeriod = GetReadPeriod(register_data);
        return res;
    }

    TTitleTranslations Translate(const std::string& name, bool idIsDefined, const TLoadingContext& context)
    {
        TTitleTranslations res;
        if (context.translations) {
            // Find translation for the name. Iterate through languages
            for (auto it = context.translations->begin(); it != context.translations->end(); ++it) {
                auto lang = it.name();
                auto translatedName = (*it)[name];
                if (!translatedName.isNull()) {
                    // Find prefix translated to the language or english translated prefix
                    auto prefixIt = context.translated_name_prefixes.find(lang);
                    if (prefixIt == context.translated_name_prefixes.end()) {
                        if (lang != "en") {
                            prefixIt = context.translated_name_prefixes.find("en");
                        }
                    }
                    // Take translation of prefix and add translated name
                    if (prefixIt != context.translated_name_prefixes.end()) {
                        res[lang] = prefixIt->second + " " + translatedName.asString();
                        continue;
                    }
                    // There isn't translated prefix
                    // Take MQTT id prefix if any and add translated name
                    if (context.mqtt_prefix.empty()) {
                        res[lang] = translatedName.asString();
                    } else {
                        res[lang] = context.mqtt_prefix + " " + translatedName.asString();
                    }
                }
            }

            for (const auto& it: context.translated_name_prefixes) {
                // There are translatied prefixes, but no translation for the name.
                // Take translated prefix and add the name as is
                if (!res.count(it.first)) {
                    res[it.first] = it.second + " " + name;
                }
            }

            // Name is different from MQTT id and there isn't english translated prefix
            // Take MQTT ID prefix if any and add the name as is
            if (!res.count("en") && idIsDefined) {
                if (context.mqtt_prefix.empty()) {
                    res["en"] = name;
                } else {
                    res["en"] = context.mqtt_prefix + " " + name;
                }
            }
            return res;
        }

        // No translations for names at all.
        // Just compose english name if it is different from MQTT id
        auto trIt = context.translated_name_prefixes.find("en");
        if (trIt != context.translated_name_prefixes.end()) {
            res["en"] = trIt->second + name;
        } else {
            if (idIsDefined) {
                res["en"] = name;
            }
        }
        return res;
    }

    void LoadSimpleChannel(TDeviceConfig* device_config,
                           const Json::Value& channel_data,
                           const TLoadingContext& context)
    {
        std::string mqtt_channel_name(channel_data["name"].asString());
        bool idIsDefined = false;
        if (channel_data.isMember("id")) {
            mqtt_channel_name = channel_data["id"].asString();
            idIsDefined = true;
        }
        if (!context.mqtt_prefix.empty()) {
            mqtt_channel_name = context.mqtt_prefix + " " + mqtt_channel_name;
        }
        auto errorMsgPrefix = "Channel \"" + mqtt_channel_name + "\"";
        std::string default_type_str;
        std::vector<PRegisterConfig> registers;
        if (channel_data.isMember("consists_of")) {

            auto read_rate_limit_ms = GetReadRateLimit(channel_data);
            auto read_period = GetReadPeriod(channel_data);

            const Json::Value& reg_data = channel_data["consists_of"];
            for (Json::ArrayIndex i = 0; i < reg_data.size(); ++i) {
                auto reg = LoadRegisterConfig(reg_data[i], *device_config, errorMsgPrefix, context);
                reg.RegisterConfig->ReadRateLimit = read_rate_limit_ms;
                reg.RegisterConfig->ReadPeriod = read_period;
                registers.push_back(reg.RegisterConfig);
                if (!i)
                    default_type_str = reg.DefaultControlType;
                else if (registers[i]->AccessType != registers[0]->AccessType)
                    throw TConfigParserException(("can't mix read-only, write-only and writable registers "
                                                  "in one channel -- ") +
                                                 device_config->DeviceType);
            }
        } else {
            try {
                auto reg = LoadRegisterConfig(channel_data, *device_config, errorMsgPrefix, context);
                default_type_str = reg.DefaultControlType;
                registers.push_back(reg.RegisterConfig);
            } catch (const std::exception& e) {
                LOG(Warn) << device_config->GetDescription() << " channel \"" + mqtt_channel_name
                          << "\" is ignored: " << e.what();
                return;
            }
        }

        std::string type_str(Read(channel_data, "type", default_type_str));
        if (type_str == "wo-switch") {
            type_str = "switch";
            for (auto& reg: registers) {
                reg->AccessType = TRegisterConfig::EAccessType::WRITE_ONLY;
            }
        }

        int order = device_config->NextOrderValue();
        PDeviceChannelConfig channel(
            new TDeviceChannelConfig(type_str,
                                     device_config->Id,
                                     order,
                                     (registers[0]->AccessType == TRegisterConfig::EAccessType::READ_ONLY),
                                     mqtt_channel_name,
                                     registers));

        for (const auto& it: Translate(channel_data["name"].asString(), idIsDefined, context)) {
            channel->SetTitle(it.second, it.first);
        }

        if (channel_data.isMember("max")) {
            channel->Max = GetDouble(channel_data, "max");
        }
        if (channel_data.isMember("min")) {
            channel->Min = GetDouble(channel_data, "min");
        }
        if (channel_data.isMember("on_value")) {
            if (registers.size() != 1)
                throw TConfigParserException("on_value is allowed only for single-valued controls -- " +
                                             device_config->DeviceType);
            channel->OnValue = std::to_string(GetInt(channel_data, "on_value"));
        }
        if (channel_data.isMember("off_value")) {
            if (registers.size() != 1)
                throw TConfigParserException("off_value is allowed only for single-valued controls -- " +
                                             device_config->DeviceType);
            channel->OffValue = std::to_string(GetInt(channel_data, "off_value"));
        }

        if (registers.size() == 1) {
            channel->Precision = registers[0]->RoundTo;
        }

        Get(channel_data, "units", channel->Units);

        device_config->AddChannel(channel);
    }

    void LoadChannel(TDeviceConfig* device_config, const Json::Value& channel_data, const TLoadingContext& context);

    void LoadSetupItem(TDeviceConfig* device_config, const Json::Value& item_data, const TLoadingContext& context);

    void LoadSubdeviceChannel(TDeviceConfig* device_config,
                              const Json::Value& channel_data,
                              const TLoadingContext& context)
    {
        int shift = 0;
        if (channel_data.isMember("shift")) {
            shift = GetInt(channel_data, "shift");
        }
        std::unique_ptr<IRegisterAddress> baseAddress(context.device_base_address.CalcNewAddress(shift, 0, 0, 0));

        TLoadingContext newContext(context.device_template_title, context.factory, *baseAddress);
        newContext.translations = context.translations;
        auto name = channel_data["name"].asString();
        newContext.name_prefix = name;
        if (!context.name_prefix.empty()) {
            newContext.name_prefix = context.name_prefix + " " + newContext.name_prefix;
        }

        newContext.mqtt_prefix = name;
        bool idIsDefined = false;
        if (channel_data.isMember("id")) {
            newContext.mqtt_prefix = channel_data["id"].asString();
            idIsDefined = true;
        }

        // Empty id is used if we don't want to add channel name to resulting MQTT topic name
        // This case we also don't add translation to resulting translated channel name
        if (!(idIsDefined && newContext.mqtt_prefix.empty())) {
            newContext.translated_name_prefixes = Translate(name, idIsDefined, context);
        }

        if (!context.mqtt_prefix.empty()) {
            if (newContext.mqtt_prefix.empty()) {
                newContext.mqtt_prefix = context.mqtt_prefix;
            } else {
                newContext.mqtt_prefix = context.mqtt_prefix + " " + newContext.mqtt_prefix;
            }
        }

        newContext.stride = Read(channel_data, "stride", 0);
        if (channel_data.isMember("setup")) {
            for (const auto& setupItem: channel_data["setup"])
                LoadSetupItem(device_config, setupItem, newContext);
        }

        if (channel_data.isMember("channels")) {
            for (const auto& ch: channel_data["channels"]) {
                LoadChannel(device_config, ch, newContext);
            }
        }
    }

    void LoadChannel(TDeviceConfig* device_config, const Json::Value& channel_data, const TLoadingContext& context)
    {
        if (channel_data.isMember("enabled") && !channel_data["enabled"].asBool()) {
            return;
        }

        if (channel_data.isMember("device_type")) {
            LoadSubdeviceChannel(device_config, channel_data, context);
        } else {
            LoadSimpleChannel(device_config, channel_data, context);
        }
    }

    void LoadSetupItem(TDeviceConfig* device_config, const Json::Value& item_data, const TLoadingContext& context)
    {
        std::string name(Read(item_data, "title", std::string("<unnamed>")));
        if (!context.name_prefix.empty()) {
            name = context.name_prefix + " " + name;
        }
        auto reg = LoadRegisterConfig(item_data, *device_config, "Setup item \"" + name + "\"", context);
        const auto& valueItem = item_data["value"];
        // libjsoncpp uses format "%.17g" in asString() and outputs strings with additional small numbers
        auto value = valueItem.isDouble() ? WBMQTT::StringFormat("%.15g", valueItem.asDouble()) : valueItem.asString();
        device_config->AddSetupItem(PDeviceSetupItemConfig(new TDeviceSetupItemConfig(name, reg.RegisterConfig, value)),
                                    context.device_template_title);
    }

    void LoadDeviceTemplatableConfigPart(TDeviceConfig* device_config,
                                         const Json::Value& device_data,
                                         PRegisterTypeMap registerTypes,
                                         const TLoadingContext& context)
    {
        device_config->TypeMap = registerTypes;

        if (device_data.isMember("setup")) {
            for (const auto& setupItem: device_data["setup"])
                LoadSetupItem(device_config, setupItem, context);
        }

        if (device_data.isMember("password")) {
            device_config->Password.clear();
            for (const auto& passwordItem: device_data["password"])
                device_config->Password.push_back(ToInt(passwordItem, "password item"));
        }

        if (device_data.isMember("delay_ms")) {
            LOG(Warn) << "\"delay_ms\" is not supported, use \"frame_timeout_ms\" instead";
        }

        Get(device_data, "frame_timeout_ms", device_config->FrameTimeout);
        if (device_config->FrameTimeout.count() < 0) {
            device_config->FrameTimeout = DefaultFrameTimeout;
        }
        Get(device_data, "response_timeout_ms", device_config->ResponseTimeout);
        Get(device_data, "device_timeout_ms", device_config->DeviceTimeout);
        Get(device_data, "device_max_fail_cycles", device_config->DeviceMaxFailCycles);
        Get(device_data, "max_reg_hole", device_config->MaxRegHole);
        Get(device_data, "max_bit_hole", device_config->MaxBitHole);
        Get(device_data, "max_read_registers", device_config->MaxReadRegisters);
        Get(device_data, "min_read_registers", device_config->MinReadRegisters);
        Get(device_data, "guard_interval_us", device_config->RequestDelay);
        Get(device_data, "stride", device_config->Stride);
        Get(device_data, "shift", device_config->Shift);
        Get(device_data, "access_level", device_config->AccessLevel);

        if (device_data.isMember("channels")) {
            for (const auto& channel_data: device_data["channels"]) {
                LoadChannel(device_config, channel_data, context);
            }
        }
    }

    void LoadDevice(PPortConfig port_config,
                    const Json::Value& device_data,
                    const std::string& default_id,
                    TTemplateMap& templates,
                    TSerialDeviceFactory& deviceFactory)
    {
        if (device_data.isMember("enabled") && !device_data["enabled"].asBool())
            return;

        port_config->AddDevice(deviceFactory.CreateDevice(device_data, default_id, port_config, templates));
    }

    PPort OpenSerialPort(const Json::Value& port_data, PRPCConfig rpcConfig)
    {
        TSerialPortSettings settings(port_data["path"].asString());

        Get(port_data, "baud_rate", settings.BaudRate);

        if (port_data.isMember("parity"))
            settings.Parity = port_data["parity"].asCString()[0];

        Get(port_data, "data_bits", settings.DataBits);
        Get(port_data, "stop_bits", settings.StopBits);

        PPort port = std::make_shared<TSerialPort>(settings);

        rpcConfig->AddSerialPort(port, settings);

        return port;
    }

    PPort OpenTcpPort(const Json::Value& port_data, PRPCConfig rpcConfig)
    {
        TTcpPortSettings settings(port_data["address"].asString(), GetInt(port_data, "port"));

        PPort port = std::make_shared<TTcpPort>(settings);

        rpcConfig->AddTCPPort(port, settings);

        return port;
    }

    void LoadPort(PHandlerConfig handlerConfig,
                  const Json::Value& port_data,
                  const std::string& id_prefix,
                  TTemplateMap& templates,
                  PRPCConfig rpcConfig,
                  TSerialDeviceFactory& deviceFactory,
                  TPortFactoryFn portFactory)
    {
        if (port_data.isMember("enabled") && !port_data["enabled"].asBool())
            return;

        auto port_config = make_shared<TPortConfig>();

        Get(port_data, "response_timeout_ms", port_config->ResponseTimeout);
        Get(port_data, "guard_interval_us", port_config->RequestDelay);
        port_config->ReadRateLimit = GetReadRateLimit(port_data);

        auto port_type = port_data.get("port_type", "serial").asString();

        Get(port_data, "connection_timeout_ms", port_config->OpenCloseSettings.MaxFailTime);
        Get(port_data, "connection_max_fail_cycles", port_config->OpenCloseSettings.ConnectionMaxFailCycles);

        std::tie(port_config->Port, port_config->IsModbusTcp) = portFactory(port_data, rpcConfig);

        const Json::Value& array = port_data["devices"];
        for (Json::Value::ArrayIndex index = 0; index < array.size(); ++index)
            LoadDevice(port_config, array[index], id_prefix + std::to_string(index), templates, deviceFactory);

        handlerConfig->AddPortConfig(port_config);
    }

    void CheckNesting(const Json::Value& root, size_t nestingLevel, ITemplateMap& templates)
    {
        if (nestingLevel > 5) {
            throw TConfigParserException(
                "Too deep subdevices nesting. This could be caused by cyclic subdevice dependencies");
        }
        for (const auto& ch: root["device"]["channels"]) {
            if (ch.isMember("device_type")) {
                CheckNesting(templates.GetTemplate(ch["device_type"].asString()).Schema, nestingLevel + 1, templates);
            }
            if (ch.isMember("oneOf")) {
                for (const auto& subdeviceType: ch["oneOf"]) {
                    CheckNesting(templates.GetTemplate(subdeviceType.asString()).Schema, nestingLevel + 1, templates);
                }
            }
        }
    }
}

std::string DecorateIfNotEmpty(const std::string& prefix, const std::string& str, const std::string& postfix)
{
    if (str.empty()) {
        return std::string();
    }
    return prefix + str + postfix;
}

void SetIfExists(Json::Value& dst, const std::string& dstKey, const Json::Value& src, const std::string& srcKey)
{
    if (src.isMember(srcKey)) {
        dst[dstKey] = src[srcKey];
    }
}

std::pair<PPort, bool> DefaultPortFactory(const Json::Value& port_data, PRPCConfig rpcConfig)
{
    auto port_type = port_data.get("port_type", "serial").asString();
    if (port_type == "serial") {
        return {OpenSerialPort(port_data, rpcConfig), false};
    }
    if (port_type == "tcp") {
        return {OpenTcpPort(port_data, rpcConfig), false};
    }
    if (port_type == "modbus tcp") {
        return {OpenTcpPort(port_data, rpcConfig), true};
    }
    throw TConfigParserException("invalid port_type: '" + port_type + "'");
}

TTemplateMap::TTemplateMap(const std::string& templatesDir,
                           const Json::Value& templateSchema,
                           bool passInvalidTemplates)
    : Validator(new WBMQTT::JSON::TValidator(templateSchema))
{
    AddTemplatesDir(templatesDir, passInvalidTemplates);
}

std::string TTemplateMap::GetDeviceType(const std::string& templatePath) const
{
    const char deviceTypeKey[] = "\"device_type\"";
    std::ifstream file;
    OpenWithException(file, templatePath);
    std::string line;
    // Search device type declaration in first 5 lines
    for (auto n = 0; n < 5; ++n) {
        std::getline(file, line);
        auto pos = line.find(deviceTypeKey);
        if (pos != std::string::npos) {
            pos += sizeof(deviceTypeKey);
            pos = line.find("\"", pos);
            if (pos != std::string::npos) {
                ++pos;
                auto end = line.find("\"", pos);
                if (end != std::string::npos) {
                    return line.substr(pos, end - pos);
                }
            }
        }
    }
    throw std::runtime_error(templatePath + " doesn't contain device type declaration");
}

void TTemplateMap::AddTemplatesDir(const std::string& templatesDir, bool passInvalidTemplates)
{
    IterateDirByPattern(
        templatesDir,
        ".json",
        [&](const std::string& filepath) {
            if (!EndsWith(filepath, ".json")) {
                return false;
            }
            struct stat filestat;
            if (stat(filepath.c_str(), &filestat) || S_ISDIR(filestat.st_mode)) {
                return false;
            }
            try {
                Json::Value root = WBMQTT::JSON::Parse(filepath);
                TemplateFiles[root["device_type"].asString()] = filepath;
            } catch (const std::exception& e) {
                if (passInvalidTemplates) {
                    LOG(Error) << "Failed to parse " << filepath << "\n" << e.what();
                    return false;
                }
                throw;
            }
            return false;
        },
        true);
}

Json::Value TTemplateMap::Validate(const std::string& deviceType, const std::string& filePath)
{
    Json::Value root(WBMQTT::JSON::Parse(filePath));
    try {
        Validator->Validate(root);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error("File: " + filePath + " error: " + e.what());
    }
    // Check that channels refer to valid subdevices and they are not nested too deep
    if (root["device"].isMember("subdevices")) {
        TSubDevicesTemplateMap subdevices(deviceType, root["device"]);
        CheckNesting(root, 0, subdevices);
    }
    return root;
}

std::shared_ptr<TDeviceTemplate> TTemplateMap::GetTemplatePtr(const std::string& deviceType)
{
    if (!Validator) {
        throw std::runtime_error("Can't find validator for device templates");
    }
    try {
        return ValidTemplates.at(deviceType);
    } catch (const std::out_of_range&) {
        std::string filePath;
        try {
            filePath = TemplateFiles.at(deviceType);
        } catch (const std::out_of_range&) {
            throw std::runtime_error("Can't find template for '" + deviceType + "'");
        }
        Json::Value root(Validate(deviceType, filePath));
        TemplateFiles.erase(filePath);
        auto deviceTypeTitle = deviceType;
        Get(root, "title", deviceTypeTitle);
        auto deviceTemplate = std::make_shared<TDeviceTemplate>(deviceType, deviceTypeTitle, root["device"]);
        Get(root, "deprecated", deviceTemplate->IsDeprecated);
        Get(root, "group", deviceTemplate->Group);
        ValidTemplates.insert({deviceType, deviceTemplate});
        return deviceTemplate;
    }
}

const TDeviceTemplate& TTemplateMap::GetTemplate(const std::string& deviceType)
{
    return *GetTemplatePtr(deviceType);
}

std::vector<std::shared_ptr<TDeviceTemplate>> TTemplateMap::GetTemplatesOrderedByName()
{
    std::vector<std::shared_ptr<TDeviceTemplate>> templates;
    for (const auto& file: TemplateFiles) {
        try {
            templates.push_back(GetTemplatePtr(file.first));
        } catch (const std::exception& e) {
            LOG(Error) << e.what();
        }
    }
    std::sort(templates.begin(), templates.end(), [](auto p1, auto p2) { return p1->Title < p2->Title; });
    return templates;
}

std::vector<std::string> TTemplateMap::GetDeviceTypes() const
{
    std::vector<std::string> res;
    for (const auto& elem: TemplateFiles) {
        res.push_back(elem.first);
    }
    return res;
}

TSubDevicesTemplateMap::TSubDevicesTemplateMap(const std::string& deviceType, const Json::Value& device)
    : DeviceType(deviceType)
{
    if (device.isMember("subdevices")) {
        AddSubdevices(device["subdevices"]);

        // Check that channels refer to valid subdevices
        for (const auto& subdeviceTemplate: Templates) {
            for (const auto& ch: subdeviceTemplate.second.Schema["channels"]) {
                if (ch.isMember("device_type")) {
                    GetTemplate(ch["device_type"].asString());
                }
                if (ch.isMember("oneOf")) {
                    for (const auto& subdeviceType: ch["oneOf"]) {
                        GetTemplate(subdeviceType.asString());
                    }
                }
            }
        }
    }
}

void TSubDevicesTemplateMap::AddSubdevices(const Json::Value& subdevicesArray)
{
    for (auto& dev: subdevicesArray) {
        auto deviceType = dev["device_type"].asString();
        if (Templates.count(deviceType)) {
            LOG(Warn) << "Device type '" << DeviceType << "'. Duplicate subdevice type '" << deviceType << "'";
        } else {
            auto deviceTypeTitle = deviceType;
            Get(dev, "title", deviceTypeTitle);
            Templates.insert({deviceType, {deviceType, deviceTypeTitle, dev["device"]}});
        }
    }
}

const TDeviceTemplate& TSubDevicesTemplateMap::GetTemplate(const std::string& deviceType)
{
    try {
        return Templates.at(deviceType);
    } catch (...) {
        throw std::runtime_error("Device type '" + DeviceType + "'. Can't find template for subdevice '" + deviceType +
                                 "'");
    }
}

std::vector<std::string> TSubDevicesTemplateMap::GetDeviceTypes() const
{
    std::vector<std::string> res;
    for (const auto& elem: Templates) {
        res.push_back(elem.first);
    }
    return res;
}

Json::Value LoadConfigTemplatesSchema(const std::string& templateSchemaFileName, const Json::Value& configSchema)
{
    Json::Value schema = WBMQTT::JSON::Parse(templateSchemaFileName);
    AppendParams(schema["definitions"], configSchema["definitions"]);
    return schema;
}

// {
//   "allOf": [
//     { "$ref": "#/definitions/deviceProperties" },
//     { "$ref": "#/definitions/common_channels" },
//     { "$ref": "#/definitions/common_setup" },
//     { "$ref": "#/definitions/slave_id" }
//   ],
//   "properties": {
//     "protocol": {
//       "type": "string",
//       "enum": ["fake"]
//     }
//   },
//   "required": ["protocol", "slave_id"]
// }
void AddFakeDeviceType(Json::Value& configSchema)
{
    Json::Value ar(Json::arrayValue);
    Json::Value v;
    v["$ref"] = "#/definitions/deviceProperties";
    ar.append(v);
    v["$ref"] = "#/definitions/common_channels";
    ar.append(v);
    v["$ref"] = "#/definitions/common_setup";
    ar.append(v);
    v["$ref"] = "#/definitions/slave_id";
    ar.append(v);

    Json::Value res;
    res["allOf"] = ar;

    res["properties"]["protocol"]["type"] = "string";
    ar.clear();
    ar.append("fake");
    res["properties"]["protocol"]["enum"] = ar;

    ar.clear();
    ar.append("protocol");
    ar.append("slave_id");
    res["required"] = ar;

    configSchema["definitions"]["device"]["oneOf"].append(res);
}

void AddRegisterType(Json::Value& configSchema, const std::string& registerType)
{
    configSchema["definitions"]["reg_type"]["enum"].append(registerType);
}

Json::Value LoadConfigSchema(const std::string& schemaFileName)
{
    return Parse(schemaFileName);
}

PHandlerConfig LoadConfig(const std::string& configFileName,
                          TSerialDeviceFactory& deviceFactory,
                          const Json::Value& baseConfigSchema,
                          TTemplateMap& templates,
                          PRPCConfig rpcConfig,
                          TPortFactoryFn portFactory)
{
    PHandlerConfig handlerConfig(new THandlerConfig);
    Json::Value Root(Parse(configFileName));

    try {
        ValidateConfig(Root, deviceFactory, baseConfigSchema, templates);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error("File: " + configFileName + " error: " + e.what());
    }

    // wb6 - single core - max 100 registers per second
    // wb7 - 4 cores - max 800 registers per second
    handlerConfig->LowPriorityRegistersRateLimit = (1 == get_nprocs_conf()) ? 100 : 800;
    Get(Root, "rate_limit", handlerConfig->LowPriorityRegistersRateLimit);

    Get(Root, "debug", handlerConfig->Debug);

    auto maxUnchangedInterval = DefaultMaxUnchangedInterval;
    Get(Root, "max_unchanged_interval", maxUnchangedInterval);
    if (maxUnchangedInterval.count() >= 0 && maxUnchangedInterval < MaxUnchangedIntervalLowLimit) {
        LOG(Warn) << "\"max_unchanged_interval\" is set to " << MaxUnchangedIntervalLowLimit.count() << " instead of "
                  << maxUnchangedInterval.count();
        maxUnchangedInterval = MaxUnchangedIntervalLowLimit;
    }
    handlerConfig->PublishParameters.Set(maxUnchangedInterval.count());

    const Json::Value& array = Root["ports"];
    for (Json::Value::ArrayIndex index = 0; index < array.size(); ++index) {
        // old default prefix for compat
        LoadPort(handlerConfig,
                 array[index],
                 "wb-modbus-" + std::to_string(index) + "-",
                 templates,
                 rpcConfig,
                 deviceFactory,
                 portFactory);
    }

    return handlerConfig;
}

void TPortConfig::AddDevice(PSerialDevice device)
{
    // try to find duplicate of this device
    for (auto dev: Devices) {
        if (dev->Protocol() == device->Protocol()) {
            if (dev->Protocol()->IsSameSlaveId(dev->DeviceConfig()->SlaveId, device->DeviceConfig()->SlaveId)) {
                stringstream ss;
                ss << "id \"" << device->DeviceConfig()->SlaveId << "\" of device \"" << device->DeviceConfig()->Name
                   << "\" is already set to device \"" + device->DeviceConfig()->Name + "\"";
                throw TConfigParserException(ss.str());
            }
        }
    }

    Devices.push_back(device);
}

TDeviceChannelConfig::TDeviceChannelConfig(const std::string& type,
                                           const std::string& deviceId,
                                           int order,
                                           bool readOnly,
                                           const std::string& mqttId,
                                           const std::vector<PRegisterConfig>& regs)
    : MqttId(mqttId),
      Type(type),
      DeviceId(deviceId),
      Order(order),
      ReadOnly(readOnly),
      RegisterConfigs(regs)
{}

const std::string& TDeviceChannelConfig::GetName() const
{
    auto it = Titles.find("en");
    if (it != Titles.end()) {
        return it->second;
    }
    return MqttId;
}

const TTitleTranslations& TDeviceChannelConfig::GetTitles() const
{
    return Titles;
}

void TDeviceChannelConfig::SetTitle(const std::string& name, const std::string& lang)
{
    if (!lang.empty()) {
        Titles[lang] = name;
    }
}

TDeviceSetupItemConfig::TDeviceSetupItemConfig(const std::string& name, PRegisterConfig reg, const std::string& value)
    : Name(name),
      RegisterConfig(reg),
      Value(value)
{
    try {
        RawValue = ConvertToRawValue(*reg, Value);
    } catch (const std::exception& e) {
        throw TConfigParserException("\"" + name + "\" bad value \"" + value + "\"");
    }
}

const std::string& TDeviceSetupItemConfig::GetName() const
{
    return Name;
}

const std::string& TDeviceSetupItemConfig::GetValue() const
{
    return Value;
}

TRegisterValue TDeviceSetupItemConfig::GetRawValue() const
{
    return RawValue;
}

PRegisterConfig TDeviceSetupItemConfig::GetRegisterConfig() const
{
    return RegisterConfig;
}

TDeviceConfig::TDeviceConfig(const std::string& name, const std::string& slave_id, const std::string& protocol)
    : Name(name),
      SlaveId(slave_id),
      Protocol(protocol)
{}

int TDeviceConfig::NextOrderValue() const
{
    return DeviceChannelConfigs.size() + 1;
}

void TDeviceConfig::AddChannel(PDeviceChannelConfig channel)
{
    DeviceChannelConfigs.push_back(channel);
}

void TDeviceConfig::AddSetupItem(PDeviceSetupItemConfig item, const std::string& deviceTemplateName)
{
    auto addrIt = SetupItemsByAddress.find(item->GetRegisterConfig()->GetAddress().ToString());
    if (addrIt != SetupItemsByAddress.end()) {
        std::stringstream ss;
        ss << "Setup command \"" << item->GetName() << "\" with address " << item->GetRegisterConfig()->GetAddress();
        if (!deviceTemplateName.empty()) {
            ss << " from device template \"" << deviceTemplateName << "\"";
        }
        ss << " has a duplicate command \"" << addrIt->second->GetName() << "\" ";
        if (item->GetValue() == addrIt->second->GetValue()) {
            ss << "with the same register value.";
        } else {
            ss << "with different register value. ";
            if (deviceTemplateName.empty()) {
                ss << "It could lead to unexpected device operation";
            } else {
                ss << "IT WILL BREAK TEMPLATE OPERATION";
            }
        }
        LOG(Warn) << ss.str();
    } else {
        SetupItemsByAddress.insert({item->GetRegisterConfig()->GetAddress().ToString(), item});
        SetupItemConfigs.push_back(item);
    }
}

std::string TDeviceConfig::GetDescription() const
{
    return "Device " + Name + " " + Id + DecorateIfNotEmpty(" (", DeviceType, ")") +
           DecorateIfNotEmpty(", protocol: ", Protocol);
}

void THandlerConfig::AddPortConfig(PPortConfig portConfig)
{
    PortConfigs.push_back(portConfig);
}

TConfigParserException::TConfigParserException(const std::string& message)
    : std::runtime_error("Error parsing config file: " + message)
{}

bool IsSubdeviceChannel(const Json::Value& channelSchema)
{
    return (channelSchema.isMember("oneOf") || channelSchema.isMember("device_type"));
}

void AppendParams(Json::Value& dst, const Json::Value& src)
{
    for (auto it = src.begin(); it != src.end(); ++it) {
        dst[it.name()] = *it;
    }
}

std::string GetProtocolName(const Json::Value& deviceDescription)
{
    std::string p;
    Get(deviceDescription, "protocol", p);
    if (p.empty()) {
        p = DefaultProtocol;
    }
    return p;
}

TDeviceTemplate::TDeviceTemplate(const std::string& type, const std::string title, const Json::Value& schema)
    : Type(type),
      Title(title),
      Schema(schema)
{}

void TSerialDeviceFactory::RegisterProtocol(PProtocol protocol, IDeviceFactory* deviceFactory)
{
    Protocols.insert(std::make_pair(protocol->GetName(), std::make_pair(protocol, deviceFactory)));
}

PProtocol TSerialDeviceFactory::GetProtocol(const std::string& name)
{
    auto it = Protocols.find(name);
    if (it == Protocols.end())
        throw TSerialDeviceException("unknown protocol: " + name);
    return it->second.first;
}

const std::string& TSerialDeviceFactory::GetCommonDeviceSchemaRef(const std::string& protocolName) const
{
    auto it = Protocols.find(protocolName);
    if (it == Protocols.end())
        throw TSerialDeviceException("unknown protocol: " + protocolName);
    return it->second.second->GetCommonDeviceSchemaRef();
}

const std::string& TSerialDeviceFactory::GetCustomChannelSchemaRef(const std::string& protocolName) const
{
    auto it = Protocols.find(protocolName);
    if (it == Protocols.end())
        throw TSerialDeviceException("unknown protocol: " + protocolName);
    return it->second.second->GetCustomChannelSchemaRef();
}

PSerialDevice TSerialDeviceFactory::CreateDevice(const Json::Value& deviceConfig,
                                                 const std::string& defaultId,
                                                 PPortConfig portConfig,
                                                 TTemplateMap& templates)
{
    TDeviceConfigLoadParams params;

    const auto* cfg = &deviceConfig;
    unique_ptr<Json::Value> mergedConfig;
    if (deviceConfig.isMember("device_type")) {
        auto deviceType = deviceConfig["device_type"].asString();
        const auto& deviceTemplate = templates.GetTemplate(deviceType);
        params.DeviceTemplateTitle = deviceTemplate.Title;
        mergedConfig = std::make_unique<Json::Value>(
            MergeDeviceConfigWithTemplate(deviceConfig, deviceType, deviceTemplate.Schema));
        cfg = mergedConfig.get();
        params.Translations = &deviceTemplate.Schema["translations"];
    }
    std::string protocolName = DefaultProtocol;
    Get(*cfg, "protocol", protocolName);

    if (portConfig->IsModbusTcp) {
        if (!GetProtocol(protocolName)->IsModbus()) {
            throw TSerialDeviceException("Protocol \"" + protocolName + "\" is not compatible with Modbus TCP");
        }
        protocolName += "-tcp";
    }

    auto it = Protocols.find(protocolName);
    if (it == Protocols.end()) {
        throw TSerialDeviceException("unknown protocol: " + protocolName);
    }

    auto protocol = it->second.first;
    const auto& deviceFactory = *it->second.second;

    params.DefaultId = defaultId;
    params.DefaultRequestDelay = portConfig->RequestDelay;
    params.PortResponseTimeout = portConfig->ResponseTimeout;
    params.DefaultReadRateLimit = portConfig->ReadRateLimit;
    auto baseDeviceConfig = LoadBaseDeviceConfig(*cfg, protocol, deviceFactory, params);

    return deviceFactory.CreateDevice(*cfg, baseDeviceConfig, portConfig->Port, protocol);
}

std::vector<std::string> TSerialDeviceFactory::GetProtocolNames() const
{
    std::vector<std::string> res;
    for (const auto& bucket: Protocols) {
        res.emplace_back(bucket.first);
    }
    return res;
}

IDeviceFactory::IDeviceFactory(std::unique_ptr<IRegisterAddressFactory> registerAddressFactory,
                               const std::string& commonDeviceSchemaRef,
                               const std::string& customChannelSchemaRef)
    : CommonDeviceSchemaRef(commonDeviceSchemaRef),
      CustomChannelSchemaRef(customChannelSchemaRef),
      RegisterAddressFactory(std::move(registerAddressFactory))
{}

const std::string& IDeviceFactory::GetCommonDeviceSchemaRef() const
{
    return CommonDeviceSchemaRef;
}

const std::string& IDeviceFactory::GetCustomChannelSchemaRef() const
{
    return CustomChannelSchemaRef;
}

const IRegisterAddressFactory& IDeviceFactory::GetRegisterAddressFactory() const
{
    return *RegisterAddressFactory;
}

PDeviceConfig LoadBaseDeviceConfig(const Json::Value& dev,
                                   PProtocol protocol,
                                   const IDeviceFactory& factory,
                                   const TDeviceConfigLoadParams& parameters)
{
    auto res = std::make_shared<TDeviceConfig>();

    Get(dev, "device_type", res->DeviceType);

    res->Id = Read(dev, "id", parameters.DefaultId);
    Get(dev, "name", res->Name);

    if (dev.isMember("slave_id")) {
        if (dev["slave_id"].isString())
            res->SlaveId = dev["slave_id"].asString();
        else // legacy
            res->SlaveId = std::to_string(dev["slave_id"].asInt());
    }

    TLoadingContext context(parameters.DeviceTemplateTitle,
                            factory,
                            factory.GetRegisterAddressFactory().GetBaseRegisterAddress());
    context.translations = parameters.Translations;
    LoadDeviceTemplatableConfigPart(res.get(), dev, protocol->GetRegTypes(), context);

    if (res->DeviceChannelConfigs.empty()) {
        LOG(Warn) << "the device has no channels: " + res->Name;
    }

    if (res->RequestDelay.count() == 0) {
        res->RequestDelay = parameters.DefaultRequestDelay;
    }

    if (parameters.PortResponseTimeout > res->ResponseTimeout) {
        res->ResponseTimeout = parameters.PortResponseTimeout;
    }
    if (res->ResponseTimeout.count() == -1) {
        res->ResponseTimeout = DefaultResponseTimeout;
    }

    auto read_rate_limit_ms = GetReadRateLimit(dev);
    if (!read_rate_limit_ms) {
        read_rate_limit_ms = parameters.DefaultReadRateLimit;
    }
    for (auto channel: res->DeviceChannelConfigs) {
        for (auto reg: channel->RegisterConfigs) {
            if (!reg->ReadRateLimit) {
                reg->ReadRateLimit = read_rate_limit_ms;
            }
        }
    }

    return res;
}

void RegisterProtocols(TSerialDeviceFactory& deviceFactory)
{
    TEnergomeraIecWithFastReadDevice::Register(deviceFactory);
    TEnergomeraIecModeCDevice::Register(deviceFactory);
    TIVTMDevice::Register(deviceFactory);
    TLLSDevice::Register(deviceFactory);
    TMercury200Device::Register(deviceFactory);
    TMercury230Device::Register(deviceFactory);
    TMilurDevice::Register(deviceFactory);
    TModbusDevice::Register(deviceFactory);
    TModbusIODevice::Register(deviceFactory);
    TNevaDevice::Register(deviceFactory);
    TPulsarDevice::Register(deviceFactory);
    TS2KDevice::Register(deviceFactory);
    TUnielDevice::Register(deviceFactory);
    TDlmsDevice::Register(deviceFactory);
    Dooya::TDevice::Register(deviceFactory);
    WinDeco::TDevice::Register(deviceFactory);
    Somfy::TDevice::Register(deviceFactory);
}

TRegisterBitsAddress LoadRegisterBitsAddress(const Json::Value& register_data, const std::string& jsonPropertyName)
{
    TRegisterBitsAddress res;
    const auto& addressValue = register_data[jsonPropertyName];
    if (addressValue.isString()) {
        const auto& addressStr = addressValue.asString();
        auto pos1 = addressStr.find(':');
        if (pos1 == string::npos) {
            res.Address = GetInt(register_data, jsonPropertyName);
        } else {
            auto pos2 = addressStr.find(':', pos1 + 1);

            res.Address = GetIntFromString(addressStr.substr(0, pos1), jsonPropertyName);
            auto bitOffset = stoul(addressStr.substr(pos1 + 1, pos2));

            if (bitOffset > 255) {
                throw TConfigParserException(
                    "address parsing failed: bit shift must be in range [0, 255] (address string: '" + addressStr +
                    "')");
            }
            res.BitOffset = bitOffset;
            if (pos2 != string::npos) {
                res.BitWidth = stoul(addressStr.substr(pos2 + 1));
            }
        }
    } else {
        res.Address = GetInt(register_data, jsonPropertyName);
    }

    if (register_data.isMember("string_data_size")) {
        res.BitWidth = GetInt(register_data, "string_data_size") * sizeof(char) * 8;
    }
    return res;
}

TUint32RegisterAddressFactory::TUint32RegisterAddressFactory(size_t bytesPerRegister)
    : BaseRegisterAddress(0),
      BytesPerRegister(bytesPerRegister)
{}

TRegisterDesc TUint32RegisterAddressFactory::LoadRegisterAddress(const Json::Value& regCfg,
                                                                 const IRegisterAddress& deviceBaseAddress,
                                                                 uint32_t stride,
                                                                 uint32_t registerByteWidth) const
{
    TRegisterDesc res;

    if (HasNoEmptyProperty(regCfg, SerialConfig::ADDRESS_PROPERTY_NAME)) {
        auto addr = LoadRegisterBitsAddress(regCfg, SerialConfig::ADDRESS_PROPERTY_NAME);
        res.DataOffset = addr.BitOffset;
        res.DataWidth = addr.BitWidth;
        res.Address = std::shared_ptr<IRegisterAddress>(
            deviceBaseAddress.CalcNewAddress(addr.Address, stride, registerByteWidth, BytesPerRegister));
        res.WriteAddress = res.Address;
    }
    if (HasNoEmptyProperty(regCfg, SerialConfig::WRITE_ADDRESS_PROPERTY_NAME)) {
        auto writeAddress = LoadRegisterBitsAddress(regCfg, SerialConfig::WRITE_ADDRESS_PROPERTY_NAME);
        res.WriteAddress = std::shared_ptr<IRegisterAddress>(
            deviceBaseAddress.CalcNewAddress(writeAddress.Address, stride, registerByteWidth, BytesPerRegister));
    }
    return res;
}

const IRegisterAddress& TUint32RegisterAddressFactory::GetBaseRegisterAddress() const
{
    return BaseRegisterAddress;
}

TRegisterDesc TStringRegisterAddressFactory::LoadRegisterAddress(const Json::Value& regCfg,
                                                                 const IRegisterAddress& deviceBaseAddress,
                                                                 uint32_t stride,
                                                                 uint32_t registerByteWidth) const
{
    TRegisterDesc res;
    if (HasNoEmptyProperty(regCfg, SerialConfig::ADDRESS_PROPERTY_NAME)) {
        res.Address = std::make_shared<TStringRegisterAddress>(regCfg[SerialConfig::ADDRESS_PROPERTY_NAME].asString());
        res.WriteAddress = res.Address;
    }
    if (HasNoEmptyProperty(regCfg, SerialConfig::WRITE_ADDRESS_PROPERTY_NAME)) {
        res.WriteAddress =
            std::make_shared<TStringRegisterAddress>(regCfg[SerialConfig::WRITE_ADDRESS_PROPERTY_NAME].asString());
    }
    return res;
}

const IRegisterAddress& TStringRegisterAddressFactory::GetBaseRegisterAddress() const
{
    return BaseRegisterAddress;
}

bool HasNoEmptyProperty(const Json::Value& regCfg, const std::string& propertyName)
{
    return regCfg.isMember(propertyName) &&
           !(regCfg[propertyName].isString() && regCfg[propertyName].asString().empty());
}
