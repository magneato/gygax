# gygax (Python)

Bindings for the Gygax neuromorphic simulator and SatLink satellite tracker, and a client for the Gygax service. Python 3.9+, numpy optional.

```bash
cmake --build build --target gygax_c
export GYGAX_LIB=$PWD/build/libgygax_c.so
PYTHONPATH=python python3 -m unittest discover -s python/tests
```

```python
from gygax import Client, neuro

with neuro.Network(dt_ms=0.1, seed=1) as net:
    pop = net.add_lif("n", 1)
    pop.set_bias(20.0)
    net.run(1000)
    print(pop.mean_rate_hz)

client = Client("http://127.0.0.1:1984", token="...")
print(client.chat("hello", model=client.models()[0]))
```

`gygax.satlink` tracks satellites: SGP4/SDP4 from two-line elements, look angles, Doppler, passes, Hamlib `rigctld` control and an `rtl_tcp` SDR client (`RtlTcp`), with a Skyfield-shaped subset for existing scripts; see [../docs/SATLINK.md](../docs/SATLINK.md).

```python
from gygax import satlink

iss = satlink.Satellite(line1, line2, "ISS (ZARYA)")
home = satlink.Observer(51.4779, -0.0015, 46)
ts = satlink.load.timescale()
for p in iss.passes(home, ts.now(), ts.now().unix + 86400, min_elevation_deg=10):
    print(p.rise.utc_iso(), "%.1f°" % p.max_elevation_deg)
```

The client also covers devices and logistics (`client.logistics.record("-3 ba99x drone=quadcopter")`). `gygax.Extension` builds tool servers the service can register, and `gygax.LocalService` starts a local `gygax serve` for development; see [../docs/EXTENSIONS.md](../docs/EXTENSIONS.md) and [../docs/LOGISTICS.md](../docs/LOGISTICS.md).

`cmake --build build` stages the package with its native library in `build/python`, so `PYTHONPATH=build/python` is enough.

The client uses only the standard library. The native library is loaded lazily, so `gygax.Client` works without it.
