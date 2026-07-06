if (CONFIG_NICOLA AND ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
  target_sources(app PRIVATE src/behavior_nicola.c)
endif()
