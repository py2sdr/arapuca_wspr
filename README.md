# Arapuca WSPR

A headless WSPR and FST4W receiver daemon for Linux. It was developed to run
as an unattended receiver without a graphical interface on a Raspberry Pi. The
program receives 48 kHz 16-bit audio samples via IP multicast, decodes weak
signal spots using `wsprd` and `jt9`, and uploads the results to
[WSPRNet](https://wsprnet.org).

The name *Arapuca* comes from a traditional Brazilian bird trap — here
repurposed as a trap for weak radio signals from around the world. No birds
were harmed — we do not endorse capturing birds! Only signals.

## Features

- Dual-mode decoding: WSPR and FST4W
- Receives audio via IP multicast (UDP)
- Time-aligned 2-minute recording windows
- Accepts 48 kHz or 12 kHz input sample rates
- Automatic 4:1 downsampling when using 48 kHz input
- Spot upload to WSPRNet with retry logic
- Sun elevation and DX distance logging
- Configurable log directory

## Dependencies

- Qt 5 (Core, Network)
- [wsprd](https://sourceforge.net/projects/wsjt/) — WSPR decoder (from WSJT-X)
- [jt9](https://sourceforge.net/projects/wsjt/) — FST4W decoder (from WSJT-X)

Both `wsprd` and `jt9` must be installed and available in `PATH`.

## Building

```bash
qmake
make
```

## Usage

```
rxwspr -c <callsign> -l <locator> -f <freq> -g <group> -p <port> -i <iface> [options]
```

### Required options

| Option | Description |
|--------|-------------|
| `-c, --callsign` | Station callsign |
| `-l, --locator`  | Maidenhead grid locator |
| `-f, --freq`     | RX frequency in Hz |
| `-g, --group`    | Multicast group address |
| `-p, --port`     | Multicast port (1025–65535) |
| `-i, --iface`    | Multicast network interface name |

### Optional

| Option | Description | Default |
|--------|-------------|---------|
| `-s, --samplerate` | Input sample rate: 48000 or 12000 | 12000 |
| `--deep` | Enable wsprd deep search mode | Off |
| `--no-upload` | Disable spot upload to WSPRNet | Off |
| `-d, --logdir` | Log file directory | Home directory |
| `-q, --quiet`  | Suppress stdout output | Off |

### Example

```bash
rxwspr -c PY2SDR -l GG66 -f 14095600 -g 239.0.0.11 -p 15004 -i dummy0
```

This starts the receiver on the 20m WSPR frequency, listening for multicast
audio on group `239.0.0.11`, port `15004`, interface `dummy0`.

## Log output

Each 2-minute cycle produces a separator line followed by any decoded spots:

```
260310 2004 -----------------------------------------  19.6  39.1 dB
260310 2004 -10  0.5     28.126007  `  TI4JWC EK70 30  50.7  5363 km  @  0.247
260310 2004 -26  0.2     28.126022  0  K3NUQ  FM09 23  31.2  7673 km  !
260310 2004 -13  0.7     28.126057  0  WW0WWV DN70 30  42.3  9104 km  !
260310 2004 -25  0.8     28.126065  1  0H5ADZ EA27 47                 !
260310 2004 -23  0.2     28.126094  0  KN6QZY CM87 23  47.6 10163 km  !
260310 2004 -24  0.4     28.126108  0  AC9YY  EN51 23  35.2  8298 km  !
260310 2004 -20  0.5     28.126120  0  KJ5SZ  EM32 23  44.1  7732 km  !
260310 2004 -22  0.4     28.126161  0  N5NBD  EM10 30  47.7  7823 km  !
```

The separator line shows the date, time (UTC), local sun elevation in degrees
(`19.6`) and received audio power level (`39.1 dB`).

The spot lines contain the following fields: date, time (UTC), signal strength
(dB), time offset (s), TX frequency (MHz), drift (Hz/min), TX callsign, TX
grid locator, TX power (dBm), sun elevation at TX location (degrees), distance
(km), and mode (`!` = WSPR, `@` = FST4W). FST4W spots include an additional
spectral spread field at the end. Balloon telemetry callsigns (starting with
`0`, `1`, or `Q`) are shown without sun elevation and distance.

Log files are written to `$HOME/rxwspr_<freq>.log` by default.

## How it works

1. Audio recording starts synchronized to the top minute at every 2 minutes
2. Audio samples arrive via multicast UDP and are accumulated for 115 seconds
3. If the input sample rate is 48 kHz, the samples are downsampled to 12 kHz; at 12 kHz they are used directly
4. The samples are written to a WAV file
4. `wsprd` and `jt9` are launched in parallel to decode WSPR and FST4W spots
5. Decoded spots are parsed, logged, and queued for upload to WSPRNet
6. A 3-second upload timer sends spots to WSPRNet with up to 10 retries

## Audio level

The received audio power level displayed in the separator line is computed as
the mean power in decibels:

$$P_{dB} = 10 \cdot \log_{10} \left( \frac{1}{N} \sum_{i=1}^{N} s_i^2 \right)$$

where $s_i$ are the 16-bit PCM samples and $N$ is the total number of samples
in the recording. This provides a quick indication of the input signal level
for each 2-minute cycle.

## License

Copyright (C) 2020 by Edson Pereira, PY2SDR

This is free software, released under the
[GNU General Public License v2](http://www.gnu.org/licenses/gpl-2.0.html).
