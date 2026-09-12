zephyr_library_named(network_manager)

# The state machine and the validator are split because the second half has no
# state: given a proposal it either produces field errors or it does not.
zephyr_library_sources(
    network_manager.c
    network_validate.c
)
