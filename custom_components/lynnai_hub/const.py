"""Constants for the LYNNAI Hub integration."""

DOMAIN = "lynnai_hub"
MANUFACTURER = "Lynnai"

CONF_HUB_ID = "hub_id"
CONF_TOKEN = "token"

DEFAULT_PORT = 8484
SSDP_ST = "urn:lynnai-com:device:hub:1"

# Dispatcher signals (formatted with the config entry id)
SIGNAL_UPDATE = "lynnai_hub_update_{}"      # some state changed: entities re-read their values
SIGNAL_NEW = "lynnai_hub_new_{}"            # a new device appeared: platforms add entities
