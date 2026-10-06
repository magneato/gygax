import sys

from gygax import Client

SIMULATION_TIMESTEP_MS = 0.1
SIMULATION_DURATION_MS = 500
SIMULATION_POPULATION_SIZE = 5
SIMULATION_INPUT_BIAS_NA = 22


def main():
    client = Client()
    if not client.health():
        print("no gygax service reachable; start one with `gygax serve`", file=sys.stderr)
        return 2
    print("models:", client.models())
    print("tools:", [t["name"] for t in client.tools()])
    print("math.eval:", client.invoke_tool("math.eval", "sqrt(2) ** 2"))
    print("chat:", client.chat("say hello", model=client.models()[0]))
    print("streamed:", "".join(client.chat_stream("stream this", model=client.models()[0])))
    agent = client.agent("!tool math.eval 6*7", model="echo")
    print("agent:", agent["status"], agent["answer"])
    sim = client.simulate(
        {
            "dt": SIMULATION_TIMESTEP_MS,
            "run_ms": SIMULATION_DURATION_MS,
            "populations": [{"name": "n", "type": "lif", "n": SIMULATION_POPULATION_SIZE,
                             "bias": SIMULATION_INPUT_BIAS_NA}],
        }
    )
    print("neuro:", sim["results"][0]["mean_rate_hz"], "Hz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
