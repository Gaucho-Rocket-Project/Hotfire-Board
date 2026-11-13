#ifndef SHELL_DAQ_H
#define SHELL_DAQ_H

class Thermocouples;
class PressureTransducers;

// Give the shell access to the existing driver.
void daq_shell_bind_thermocouples(
    Thermocouples& thermocouples
);

void daq_shell_bind_pressure(PressureTransducers& pressure);

#endif // SHELL_DAQ_H
