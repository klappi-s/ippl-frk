# Centralized logging for Trame Catalyst app
# Debug levels:
# -1: Silent (no output)
#  0: User interactions only (clicks, button presses)
#  1: Moderate debug (one line per operation with summary)
#  2: Detailed debug (all internal state, verbose)

import sys

class Logger:
    def __init__(self, level=0):
        self._level = level
    
    def set_level(self, level):
        """Set the debug level: -1 (silent), 0 (UI), 1 (moderate), 2 (detailed)"""
        self._level = int(level)
    
    def get_level(self):
        return self._level
    
    def silent(self, msg, *args):
        """Never printed (placeholder for code clarity)"""
        pass
    
    def ui(self, msg, *args):
        """Level 0+: User interaction events"""
        if self._level >= 0:
            formatted = msg.format(*args) if args else msg
            print(f"[UI] {formatted}", flush=True)
    
    def info(self, msg, *args):
        """Level 1+: Summary information (one line per operation)"""
        if self._level >= 1:
            formatted = msg.format(*args) if args else msg
            print(f"[INFO] {formatted}", flush=True)
    
    def debug(self, msg, *args):
        """Level 2+: Detailed debugging information"""
        if self._level >= 2:
            formatted = msg.format(*args) if args else msg
            print(f"[DEBUG] {formatted}", flush=True)
    
    def warn(self, msg, *args):
        """Always printed: Warnings"""
        formatted = msg.format(*args) if args else msg
        print(f"[WARN] {formatted}", file=sys.stderr, flush=True)
    
    def error(self, msg, *args):
        """Always printed: Errors"""
        formatted = msg.format(*args) if args else msg
        print(f"[ERROR] {formatted}", file=sys.stderr, flush=True)

# Global logger instance
_logger = Logger(level=0)

def set_log_level(level):
    """Set the global log level"""
    _logger.set_level(level)

def get_log_level():
    """Get the current log level"""
    return _logger.get_level()

def get_logger():
    """Get the global logger instance"""
    return _logger

# Convenience functions
def ui(msg, *args):
    _logger.ui(msg, *args)

def info(msg, *args):
    _logger.info(msg, *args)

def debug(msg, *args):
    _logger.debug(msg, *args)

def warn(msg, *args):
    _logger.warn(msg, *args)

def error(msg, *args):
    _logger.error(msg, *args)
