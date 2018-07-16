#include "serial_client.h"
#include "virtual_register.h"
#include "ir_device_query.h"
#include "ir_device_query_handler.h"
#include "ir_device_query_factory.h"

#include <unistd.h>
#include <unordered_map>
#include <iostream>


namespace {
    struct TSerialPollEntry: public TPollEntry {
        PIRDeviceQuerySet QuerySet;
        TIntervalMs       PollIntervalValue;

        TSerialPollEntry(TIntervalMs pollInterval, const PIRDeviceQuerySet & querySet)
            : QuerySet(querySet)
            , PollIntervalValue(pollInterval)
        {
            assert(!QuerySet->Queries.empty());
        }

        TIntervalMs PollInterval() const override {
            return PollIntervalValue;
        }
    };
    typedef std::shared_ptr<TSerialPollEntry> PSerialPollEntry;
};

TSerialClient::TSerialClient(PPort port)
    : Port(port),
      Active(false),
      ReadCallback([](PVirtualRegister){}),
      ErrorCallback([](PVirtualRegister){}),
      FlushNeeded(new TBinarySemaphore),
      Plan(std::make_shared<TPollPlan>([this]() { return Port->CurrentTime(); })) {}

TSerialClient::~TSerialClient()
{
    if (Active)
        Disconnect();

    // remove all registered devices
    for (auto &dev : DevicesList)
        TSerialDeviceFactory::RemoveDevice(dev);
}

PSerialDevice TSerialClient::CreateDevice(PDeviceConfig device_config)
{
    if (Active)
        throw TSerialDeviceException("can't add registers to the active client");
    if (Debug)
        std::cerr << "CreateDevice: " << device_config->Id <<
            (device_config->DeviceType.empty() ? "" : " (" + device_config->DeviceType + ")") <<
            " @ " << device_config->SlaveId << " -- protocol: " << device_config->Protocol << std::endl;

    try {
        PSerialDevice dev = TSerialDeviceFactory::CreateDevice(device_config, Port);
        DevicesList.push_back(dev);
        return dev;
    } catch (const TSerialDeviceException& e) {
        Disconnect();
        throw;
    }
}

void TSerialClient::AddRegister(PVirtualRegister reg)
{
    if (Active)
        throw TSerialDeviceException("can't add registers to the active client");

    VirtualRegisters[reg->GetDevice()].push_back(reg);

    reg->SetFlushSignal(FlushNeeded);

    if (Debug)
        std::cerr << "AddRegister: " << reg << std::endl;
}

void TSerialClient::Connect()
{
    if (Active)
        return;
    if (!VirtualRegisters.size())
        throw TSerialDeviceException("no registers defined");
    if (!Port->IsOpen())
        Port->Open();
    InitializeMemoryBlocksCache();
    GenerateReadQueries();
    Active = true;
}

void TSerialClient::Disconnect()
{
    if (Port->IsOpen())
        Port->Close();
    Active = false;
}

void TSerialClient::InitializeMemoryBlocksCache()
{
    for (const auto & device: DevicesList) {
        device->InitializeMemoryBlocksCache();
    }
}

void TSerialClient::GenerateReadQueries()
{
    std::cerr << "generating read queries..." << std::endl;
    uint32_t queryCount = 0;
    // all of this is seemingly slow but it's actually only done once
    Plan->Reset();

    // iterate over DevicesList to preserve order
    for (const auto & device: DevicesList) {
        auto itVirtualRegisters = VirtualRegisters.find(device);
        if (itVirtualRegisters == VirtualRegisters.end()) {
            continue;
        }

        const auto & virtualRegisters = itVirtualRegisters->second;

        for (const auto & pollIntervalQuerySet: TIRDeviceQueryFactory::GenerateQuerySets(virtualRegisters, EQueryOperation::Read)) {
            const auto & pollInterval = pollIntervalQuerySet.first;
            const auto & querySet = pollIntervalQuerySet.second;

            Plan->AddEntry(std::make_shared<TSerialPollEntry>(pollInterval, querySet));
            queryCount += querySet->Queries.size();
        }
    }

    std::cerr << queryCount <<  " queries generated" << std::endl;
}

void TSerialClient::MaybeUpdateErrorState(PVirtualRegister reg)
{
    if (reg->GetErrorState() != EErrorState::UnknownErrorState) {
        ErrorCallback(reg);
    }
}

bool TSerialClient::DoFlush()
{
    bool needFlush = false;

    for (const auto & device: DevicesList) {
        auto itVirtualRegisters = VirtualRegisters.find(device);
        if (itVirtualRegisters == VirtualRegisters.end()) {
            continue;
        }

        const auto & virtualRegisters = itVirtualRegisters->second;

        for (const auto & reg: virtualRegisters) {
            if (!reg->NeedToFlush())
                continue;
            PrepareToAccessDevice(device);
            reg->Flush();
            needFlush |= reg->NeedToFlush();
            if (reg->IsChanged(EPublishData::Error)) {
                MaybeUpdateErrorState(reg);
            }
        }
    }

    return needFlush;
}

bool TSerialClient::WaitForPollAndFlush()
{
    bool needFlush = false;
    // When it's time for a next poll, take measures
    // to avoid poll starvation
    if (Plan->PollIsDue()) {
        MaybeFlushAvoidingPollStarvationButDontWait(needFlush);
        return needFlush;
    }
    auto wait_until = Plan->GetNextPollTimePoint();
    while (Port->Wait(FlushNeeded, wait_until)) {
        // Don't hold the lock while flushing
        needFlush = DoFlush();
        if (Plan->PollIsDue()) {
            MaybeFlushAvoidingPollStarvationButDontWait(needFlush);
            return needFlush;
        }
    }
    return needFlush;
}

void TSerialClient::MaybeFlushAvoidingPollStarvationButDontWait(bool & needFlush)
{
    // avoid poll starvation
    int flush_count_remaining = MAX_FLUSHES_WHEN_POLL_IS_DUE;
    while (flush_count_remaining-- && FlushNeeded->TryWait())
        needFlush = DoFlush();
}

void TSerialClient::Cycle()
{
    Connect();

    Port->CycleBegin();

    bool stillNeedFlush = WaitForPollAndFlush();

    // devices whose registers were polled during this cycle and statues
    std::map<PSerialDevice, std::set<EQueryStatus>> devicesRangesStatuses;

    Plan->ProcessPending([&](const PPollEntry& entry) {
        const auto & querySet = std::dynamic_pointer_cast<TSerialPollEntry>(entry)->QuerySet;
        auto device = querySet->GetDevice();

        auto & statuses = devicesRangesStatuses[device];

        for (const auto & query: querySet->Queries) {
            if (!query->IsEnabled()) {
                continue;
            }

            if (device->GetIsDisconnected()) {
                // limited polling mode
                if (statuses.empty()) {
                    // First interaction with disconnected device within this cycle: Try to reconnect
                    if (device->HasSetupItems()) {
                        auto wrote = device->WriteSetupRegisters(false);
                        statuses.insert(wrote ? EQueryStatus::Ok : EQueryStatus::UnknownError);
                        if (!wrote) {
                            continue;
                        }
                    }
                } else {
                    // Not first interaction with disconnected device that has only errors - still disconnected
                    if (statuses.count(EQueryStatus::UnknownError) == statuses.size()) {
                        continue;
                    }
                }
            }

            PrepareToAccessDevice(device);
            device->Execute(query);

            for (const auto & virtualRegister: query->VirtualRegisters) {
                if (virtualRegister->IsChanged(EPublishData::Error)) {
                    MaybeUpdateErrorState(virtualRegister);
                }

                if (!Has(virtualRegister->GetErrorState(), EErrorState::ReadError)) {
                    ReadCallback(virtualRegister);
                }
            }
            statuses.insert(query->GetStatus());
        }

        TIRDeviceQuerySetHandler::HandleQuerySetPostExecution(querySet);

        MaybeFlushAvoidingPollStarvationButDontWait(stillNeedFlush);
    });

    for (const auto & device: DevicesList) {
        auto it = devicesRangesStatuses.find(device);
        assert(it != devicesRangesStatuses.end());
        const auto & statuses = it->second;

        if (statuses.empty()) {
            std::cerr << "invariant violation: statuses empty @ " << __func__ << std::endl;
            continue;   // this should not happen
        }

        bool deviceWasDisconnected = device->GetIsDisconnected(); // don't move after device->OnCycleEnd(...);
        {
            bool cycleFailed = statuses.count(EQueryStatus::UnknownError) == statuses.size();
            device->OnCycleEnd(!cycleFailed);
        }

        if (deviceWasDisconnected && !device->GetIsDisconnected()) {
            OnDeviceReconnect(device);
        }
    }

    for (const auto& p: DevicesList) {
        p->EndPollCycle();
    }

    if (stillNeedFlush) {
        NotifyFlushNeeded();
    }

    // Port status
    {
        bool cycleFailed = AllOf(DevicesList, [](const PSerialDevice & device){
            return device->GetIsDisconnected();
        });

        Port->CycleEnd(!cycleFailed);
    }
}

bool TSerialClient::WriteSetupRegisters(PSerialDevice dev)
{
    Connect();
    PrepareToAccessDevice(dev);
    return dev->WriteSetupRegisters();
}

void TSerialClient::SetReadCallback(const TSerialClient::TReadCallback& callback)
{
    ReadCallback = callback;
}

void TSerialClient::SetErrorCallback(const TSerialClient::TErrorCallback& callback)
{
    ErrorCallback = callback;
}

void TSerialClient::SetDebug(bool debug)
{
    Debug = debug;
    Port->SetDebug(debug);
}

bool TSerialClient::DebugEnabled() const {
    return Debug;
}

void TSerialClient::NotifyFlushNeeded()
{
    FlushNeeded->Signal();
}

void TSerialClient::PrepareToAccessDevice(PSerialDevice dev)
{
    if (dev != LastAccessedDevice) {
        LastAccessedDevice = dev;
        dev->Prepare();
    }
}

void TSerialClient::OnDeviceReconnect(PSerialDevice dev)
{
	if (Debug) {
		std::cerr << "device " << dev->ToString() << " reconnected" << std::endl;
	}

    for (const auto & virtualRegister: VirtualRegisters[dev]) {
        virtualRegister->SetEnabled(true);
    }
}
