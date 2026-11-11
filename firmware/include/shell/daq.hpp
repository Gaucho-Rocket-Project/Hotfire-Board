#ifndef SHELL_DAQ_H
#define SHELL_DAQ_H

class Thermocouples;

// Give the shell access to the existing driver.
void daq_shell_bind_thermocouples(
    Thermocouples& thermocouples
);

#endif // SHELL_DAQ_H