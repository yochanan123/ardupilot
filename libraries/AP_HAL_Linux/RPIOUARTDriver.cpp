#include "RPIOUARTDriver.h"

#include <stdlib.h>
#include <cstdio>

#include <AP_HAL/AP_HAL.h>

#include "px4io_protocol.h"

#define RPIOUART_POLL_TIME_INTERVAL 10000

extern const AP_HAL::HAL& hal;

#define RPIOUART_DEBUG 0

#include <cassert>

#if RPIOUART_DEBUG
#define debug(fmt, args ...)  do {hal.console->printf("[RPIOUARTDriver]: %s:%d: " fmt "\n", __FUNCTION__, __LINE__, ## args); } while(0)
#define error(fmt, args ...)  do {fprintf(stderr,"%s:%d: " fmt "\n", __FUNCTION__, __LINE__, ## args); } while(0)
#else
#define debug(fmt, args ...)
#define error(fmt, args ...)
#endif

using namespace Linux;

RPIOUARTDriver::RPIOUARTDriver() :
    UARTDriver(false),
    _dev(nullptr),
    _last_update_timestamp(0),
    _external(false),
    _need_set_baud(false),
    _baudrate(0)
{
}

bool RPIOUARTDriver::sem_take_nonblocking()
{
    return _dev->get_semaphore()->take_nonblocking();
}

void RPIOUARTDriver::sem_give()
{
    _dev->get_semaphore()->give();
}

bool RPIOUARTDriver::isExternal()
{
    return _external;
}

void RPIOUARTDriver::begin(uint32_t b, uint16_t rxS, uint16_t txS)
{
    //hal.console->printf("[RPIOUARTDriver]: begin \n");

    if (device_path != NULL) {
        UARTDriver::begin(b,rxS,txS);
        if ( is_initialized()) {
            _external = true;
            return;
        }
    }

   if (rxS < 1024) {
       rxS = 2048;
   }
   if (txS < 1024) {
       txS = 2048;
   }

    _initialised = false;
    while (_in_timer) hal.scheduler->delay(1);

    _readbuf.set_size(rxS);
    _writebuf.set_size(txS);

   _dev = hal.spi->get_device("raspio");

    /* set baudrate */
    _baudrate = b;
    _need_set_baud = true;
    while (_need_set_baud) {
        hal.scheduler->delay(1);
    }

    if (_writebuf.get_size() && _readbuf.get_size()) {
        _initialised = true;
    }
}

int RPIOUARTDriver::_write_fd(const uint8_t *buf, uint16_t n)
{
    if (_external) {
        return UARTDriver::_write_fd(buf, n);
    }

    return -1;
}

int RPIOUARTDriver::_read_fd(uint8_t *buf, uint16_t n)
{
    if (_external) {
        return UARTDriver::_read_fd(buf, n);
    }

    return -1;
}

void RPIOUARTDriver::_timer_tick(void)
{
    if (_external) {
        UARTDriver::_timer_tick();
        return;
    }

    /* set the baudrate of raspilotio */
    if (_need_set_baud) {

        if (_baudrate != 0) {

            if (!_dev->get_semaphore()->take_nonblocking()) {
                return;
            }

            struct IOPacket _dma_packet_tx = {0}, _dma_packet_rx = {0};

            _dma_packet_tx.count_code = 2 | PKT_CODE_WRITE;
            _dma_packet_tx.page = PX4IO_PAGE_UART_BUFFER;
            _dma_packet_tx.offset = 0;
            _dma_packet_tx.regs[0] = _baudrate & 0xffff;
            _dma_packet_tx.regs[1] = _baudrate >> 16;
            _dma_packet_tx.crc = 0;
            _dma_packet_tx.crc = crc_packet(&_dma_packet_tx);

            _dev->transfer((uint8_t *)&_dma_packet_tx, sizeof(_dma_packet_tx),
                           (uint8_t *)&_dma_packet_rx, sizeof(_dma_packet_rx));

            hal.scheduler->delay(1);

            _dev->get_semaphore()->give();

        }

        _need_set_baud = false;
    }
    /* finish set */

    if (!_initialised) return;

    /* lower the update rate */
    if (AP_HAL::micros() - _last_update_timestamp < RPIOUART_POLL_TIME_INTERVAL) {
        return;
    }

    _in_timer = true;

    if (!_dev->get_semaphore()->take_nonblocking()) {
        return;
    }

    struct IOPacket _dma_packet_tx = {0}, _dma_packet_rx = {0};

    /* get write_buf bytes */
    uint32_t n = _writebuf.available();

    if (n > PKT_MAX_REGS * 2) {
        n = PKT_MAX_REGS * 2;
    }

    uint16_t _max_size = _baudrate / 10 / (1000000 / RPIOUART_POLL_TIME_INTERVAL);
    if (n > _max_size) {
        n = _max_size;
    }

    _writebuf.read(&((uint8_t *)_dma_packet_tx.regs)[0], n);

    _dma_packet_tx.count_code = PKT_MAX_REGS | PKT_CODE_SPIUART;
    _dma_packet_tx.page = PX4IO_PAGE_UART_BUFFER;
    _dma_packet_tx.offset = n;
    /* end get write_buf bytes */

    _dma_packet_tx.crc = 0;
    _dma_packet_tx.crc = crc_packet(&_dma_packet_tx);
    /* set raspilotio to read uart data */
    _dev->transfer((uint8_t *)&_dma_packet_tx, sizeof(_dma_packet_tx),
                   (uint8_t *)&_dma_packet_rx, sizeof(_dma_packet_rx));

    hal.scheduler->delay_microseconds(100);

    /* get uart data from raspilotio */
    _dma_packet_tx.count_code = 0 | PKT_CODE_READ;
    _dma_packet_tx.page = 0;
    _dma_packet_tx.offset = 0;
    memset( &_dma_packet_tx.regs[0], 0, PKT_MAX_REGS*sizeof(uint16_t) );
    _dma_packet_tx.crc = 0;
    _dma_packet_tx.crc = crc_packet(&_dma_packet_tx);
    _dev->transfer((uint8_t *)&_dma_packet_tx, sizeof(_dma_packet_tx),
                   (uint8_t *)&_dma_packet_rx, sizeof(_dma_packet_rx));

    hal.scheduler->delay_microseconds(100);

    /* release sem */
    _dev->get_semaphore()->give();

    /* add bytes to read buf */
    n = _readbuf.space();

    if (_dma_packet_rx.page == PX4IO_PAGE_UART_BUFFER) {

        if (n > _dma_packet_rx.offset) {
            n = _dma_packet_rx.offset;
        }

        if (n > PKT_MAX_REGS * 2) {
            n = PKT_MAX_REGS * 2;
        }

        _readbuf.write(&((uint8_t *)_dma_packet_rx.regs)[0], n);
    }

    _in_timer = false;

    _last_update_timestamp = AP_HAL::micros();
}
