#ifndef DRIVERS_SPI_DECODER_HPP
#define DRIVERS_SPI_DECODER_HPP

#include <stddef.h>
#include <stdint.h>
struct spi_config;

// Sole owner of SPI2 and the CD74HC154 address pins. All clients must use this
// interface so selection, SPI mode changes, and DMA completion are serialized.
namespace spi_decoder {
int initialize(const char** failure_stage = nullptr);
// config must have static lifetime and remain immutable (Zephyr caches its address).
int transfer(uint8_t address, const spi_config& config,
             uint8_t* tx, uint8_t* rx, size_t length,
             const char** failure_stage = nullptr);
}

#endif
