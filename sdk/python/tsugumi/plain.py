"""Convert generated objects and arrays without importing the worker host."""

def plain(value):
    if isinstance(value, bytes):
        return value.decode('utf-8')
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    if hasattr(value, 'tolist'):
        return plain(value.tolist())
    if hasattr(value, '__dict__'):
        return {k: plain(v) for k, v in vars(value).items()}
    return value

