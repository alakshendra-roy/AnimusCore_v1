from animus import EventEngine, trace

engine = EventEngine()
result = engine.process_telemetry_batch()
print('SDK Test Result:', result)


@trace(event_id=42)
def sample_traced_call(x: int) -> int:
    return x * x


print('Traced call result:', sample_traced_call(7))
