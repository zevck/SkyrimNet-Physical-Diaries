#pragma once

namespace SkyrimNetAPITest {

    /**
     * Initialize the SkyrimNet API and check version.
     * @return true if API v3+ is available
     */
    bool InitializeAPI();

    /**
     * Test if the memory/database system is ready.
     */
    void TestMemorySystem();

    /**
     * Test UUID resolution functions (FormID <-> UUID conversion).
     */
    void TestUUIDResolution();

    /**
     * Test bio template retrieval for an actor.
     * @param formId Actor FormID
     */
    void TestBioTemplate(uint32_t formId);

    /**
     * Test memory query for an actor.
     * @param formId Actor FormID
     * @param maxCount Maximum memories to retrieve
     */
    void TestMemoriesQuery(uint32_t formId, int maxCount = 5);

    /**
     * Test recent events query for an actor.
     * @param formId Actor FormID
     * @param maxCount Maximum events to retrieve
     */
    void TestRecentEvents(uint32_t formId, int maxCount = 5);

    /**
     * Test dialogue history query for an actor.
     * @param formId Actor FormID
     * @param maxExchanges Maximum exchanges to retrieve
     */
    void TestDialogueQuery(uint32_t formId, int maxExchanges = 5);

    /**
     * Test latest dialogue info query.
     */
    void TestLatestDialogue();

    /**
     * Test actor engagement statistics query.
     */
    void TestActorEngagement();

    /**
     * Test related actors query.
     * @param formId Anchor actor FormID
     */
    void TestRelatedActors(uint32_t formId);

    /**
     * Test player context query.
     */
    void TestPlayerContext();

    /**
     * Run all API tests with default parameters.
     */
    void RunAllTests();

    /**
     * Run comprehensive tests for a specific actor.
     * @param formId Actor FormID to test
     */
    void TestSpecificActor(uint32_t formId);

} // namespace SkyrimNetAPITest
