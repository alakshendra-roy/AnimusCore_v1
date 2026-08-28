from animus import EventEngine

engine = EventEngine()
result = engine.process_telemetry_batch()
print('SDK Test Result:', result)
