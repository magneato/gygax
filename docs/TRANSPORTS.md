# Device transports

All transports return `IOResult`: `0` on success, `-errno` on failure. Every one is covered by tests that run without hardware: serial devices are exercised over pseudo-terminals, GPIO over a fake sysfs tree, printers over a local TCP listener.

## Serial (`transport/serial.hpp`)

`SerialTransport` opens `/dev/tty*` devices in raw, non-blocking mode (115200 8N1 by default) and runs one reader thread. `configure` sets baud (300 to 230400), data bits, stop bits, parity and flow control (RTS/CTS, XON/XOFF); `setLine`/`getLine` drive DTR and RTS and read CTS, DSR, DCD and RI. Received bytes are buffered (1 MiB cap, oldest dropped) and read with `read`/`waitReadable`, or delivered to a callback registered with `setCallback` (in which case they are not buffered). `enumerateDevices` lists `ttyS*`, `ttyUSB*`, `ttyACM*` and macOS `cu.usb*`.

## GPIO (`transport/gpio.hpp`)

`SysfsGpioPort` drives the Linux sysfs interface (`/sys/class/gpio` or any root you pass): it exports pins on demand, sets direction, reads and writes values, and polls watched pins every few milliseconds to deliver edge callbacks with a monotonic timestamp. Only inputs and outputs exist; pull resistors and open-drain need the newer character-device API, which is not implemented. Pins it exported are unexported on shutdown.

## Hayes modem (`transport/modem.hpp`)

`HayesModem` runs AT commands over a serial handle: `initialize` (ATZ, ATE0V1Q0), `dial` (validated dial string, tone or pulse), `answer`, `hangup` (guard time, `+++`, `ATH0`), `sendData`, `receiveData`. Result codes OK, ERROR, CONNECT (with rate), NO CARRIER, BUSY, NO DIALTONE, NO ANSWER and a timeout are parsed.

## Printers (`transport/printer.hpp`)

`RawPrinter` sends raw jobs to `tcp://host[:9100]` (JetDirect) or a device file, wraps jobs in PJL (`printPjl`, job names are sanitised), and `query` reads PJL status replies from TCP printers. It sends bytes; it does not render PostScript, PCL or ESC/P for you.

## G-code (`transport/gcode.hpp`)

`GcodePrinter` speaks the Marlin host protocol over a serial handle: `M110` sync, optional `N<line> ... *<xor checksum>` framing, waiting for `ok`, automatic recovery from `Resend:` requests, firmware `Error:` surfacing, `M105` and `M114` parsing, bounded `M104`/`M140` targets, program streaming with progress callbacks and a raw `M112` emergency stop.

## Streams (`transport/streams.hpp`)

`comm::CreateUdpStream(bindPort)` and `comm::CreateTcpLineStream()` implement the `Stream` interface used by embodiments and the sim-to-real link (`host:port` destinations, JSON lines).

## Not implemented

Fax (T.30), flatbed and 3D scanners, QR/barcode decoding, infrared protocol encoding, JTAG bit-banging and the port multiplexer were placeholders that reported success without doing anything. They were removed from the build and kept in `trash/transport_old` until they have real backends and hardware-free tests.
