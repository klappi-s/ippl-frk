# Lightweight shared context for trame modules to avoid cross-file globals
# This lets us pass server/state/ctrl and runtime flags without circular imports.

from dataclasses import dataclass, field
from typing import Any, Dict, Optional

@dataclass
class Ctx:
    server: Any
    state: Any
    ctrl: Any

    # Catalyst link and active proxies
    catalyst_link: Optional[Any] = None
    active_proxies: Dict[str, Any] = field(default_factory=dict)
    steerable_proxies: list = field(default_factory=list)

    # Transport/update gates
    view_update_enabled: bool = True
    last_view_update_ts: float = 0.0
    view_update_pending_until: float = 0.0

    # Camera debounce
    camera_last_sig: Optional[tuple] = None
    camera_last_change_ts: float = 0.0

    # Optional extras for future modules
    def reset_runtime(self):
        self.view_update_enabled = True
        self.last_view_update_ts = 0.0
        self.view_update_pending_until = 0.0
        self.camera_last_sig = None
        self.camera_last_change_ts = 0.0
        # UI-scoped cadence marker preserved in state
        try:
            self.state.last_resample_update_ts = 0.0
        except Exception:
            pass
