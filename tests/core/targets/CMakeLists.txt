fc_add_catch_test(fc_target_tests
    NAME fc_targets
    LABEL fast
    SOURCES target_tests.cpp
    LIBRARIES fusioncutter_catalog
)

set(FC_TEST_GOG_GAME_IMAGE "" CACHE FILEPATH "Reviewed GOG BattlefrontII.exe used by target image tests")
set(FC_TEST_GALAXY_2017_IMAGE "" CACHE FILEPATH "Reviewed 2017 GOG GalaxyPeer.dll image")
set(FC_TEST_GALAXY_2018_IMAGE "" CACHE FILEPATH "Reviewed 2018 GOG GalaxyPeer.dll image")
set(FC_TEST_CLASSIC_BOOTSTRAP_IMAGE "" CACHE FILEPATH "Reviewed Classic Collection Battlefront.exe image")
set(FC_TEST_CLASSIC_GAME_IMAGE "" CACHE FILEPATH "Reviewed Classic Collection Battlefront2.dll image")

if(FC_TEST_GOG_GAME_IMAGE OR FC_TEST_GALAXY_2017_IMAGE OR FC_TEST_GALAXY_2018_IMAGE OR
   FC_TEST_CLASSIC_BOOTSTRAP_IMAGE OR FC_TEST_CLASSIC_GAME_IMAGE)
    # One verifier accepts image paths from CTest so locally available proprietary images remain outside the source and
    # generated fixtures.
    add_executable(fc_target_image_tests
        target_image_tests.cpp
    )

    fc_configure_project_target(fc_target_image_tests)
    fc_set_artifact_directories(fc_target_image_tests)

    target_link_libraries(fc_target_image_tests PRIVATE
        fusioncutter_catalog
    )

    # Registers only explicitly supplied images so missing local game files do not weaken the ordinary test matrix.
    function(fc_add_target_image_test name profile path)
        if(path)
            add_test(NAME ${name} COMMAND fc_target_image_tests "${profile}" "${path}")
            set_tests_properties(${name} PROPERTIES LABELS images)
        endif()
    endfunction()

    fc_add_target_image_test(fc_image_gog_game "GOGRetail_Game_59EDF52B" "${FC_TEST_GOG_GAME_IMAGE}")
    fc_add_target_image_test(fc_image_galaxy_2017 "GOGRetail_GalaxyPeer_59E6304A"
        "${FC_TEST_GALAXY_2017_IMAGE}")
    fc_add_target_image_test(fc_image_galaxy_2018 "GOGRetail_GalaxyPeer_5BBE22A6"
        "${FC_TEST_GALAXY_2018_IMAGE}")
    fc_add_target_image_test(fc_image_classic_bootstrap "ClassicCollection_Bootstrap_66702CD7"
        "${FC_TEST_CLASSIC_BOOTSTRAP_IMAGE}")
    fc_add_target_image_test(fc_image_classic_game "ClassicCollection_Game_66702CD2" "${FC_TEST_CLASSIC_GAME_IMAGE}")
endif()
