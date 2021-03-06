Feature: Check modem firmware

    The Cat Tracker should check that the correct modem firmware is running

    Scenario: Print a message

        Given the Firmware CI job "{env__JOB_ID}" has completed
        Then the Firmware CI device log for job "{env__JOB_ID}" should contain
        """
        cat_tracker: Board is running expected modem firmware version: mfw_nrf9160_1.2.1
        """