"""Exceptions with stable categories for the recorder CLI."""


class RecorderError(Exception):
    """Base class for expected recorder failures."""


class ProtocolError(RecorderError):
    """The UART stream did not satisfy the version-2 protocol."""


class TransportTimeout(RecorderError):
    """The Pico did not provide an expected frame before the deadline."""


class ModeRejected(RecorderError):
    """The Pico rejected a requested mode transition."""


class PicoError(RecorderError):
    """The Pico sent an ERROR frame while recording."""


class RecordingValidationError(RecorderError):
    """A recording or recording field is invalid."""


class StorageError(RecorderError):
    """A recording could not be safely loaded or stored."""
