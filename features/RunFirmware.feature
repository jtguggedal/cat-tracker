Feature: Run the firmware

    The Cat Tracker should run the firmware

    Scenario: The firmware should have been run

        Given the Firmware CI job "{env__JOB_ID}" has completed
        Then the Firmware CI device log for job "{env__JOB_ID}" should contain
        """
        cat_tracker:  The cat tracker has started
        cat_tracker:  Version:     {env__CAT_TRACKER_APP_VERSION}
        cat_tracker:  Client ID:   {env__JOB_ID}
        cat_tracker:  Endpoint:    {mqttEndpoint}
        """