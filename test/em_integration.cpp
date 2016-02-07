#include <stdexcept>
#include "fake_serial_port.h"
#include "em_expectations.h"
#include "serial_config.h"
#include "serial_observer.h"

class TEMIntegrationTest: public TSerialDeviceIntegrationTest, public TEMDeviceExpectations
{
protected:
    const char* ConfigPath() const { return "../config-em-test.json"; }
};

TEST_F(TEMIntegrationTest, Poll)
{
    Observer->SetUp();
    SerialPort->SetExpectedFrameTimeout(150);
    ASSERT_TRUE(!!SerialPort);

    EnqueueMilurSessionSetupResponse();
    EnqueueMilurPhaseCVoltageResponse();
    EnqueueMilurTotalConsumptionResponse();
    EnqueueMercury230SessionSetupResponse();
    EnqueueMercury230EnergyResponse1();
    EnqueueMercury230U1Response();
    EnqueueMercury230U2Response();
    EnqueueMercury230I1Response();

    Note() << "LoopOnce()";
    Observer->LoopOnce();

    SerialPort->Close();
}

// TBD: test errors
