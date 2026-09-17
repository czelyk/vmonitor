#include <stdio.h>
#include <string.h>

#include "../server/protocol.h"

static int failures = 0;

static void check(int condition, const char *message)
{
    if (condition) {
        printf("[PASS] %s\n", message);
    } else {
        printf("[FAIL] %s\n", message);
        failures++;
    }
}

static void test_valid_command(const char *input,
                               uint32_t expected_id,
                               cmd_type_t expected_type,
                               int expected_has_arg,
                               int32_t expected_arg,
                               const char *name)
{
    parsed_cmd_t cmd;
    int result;

    memset(&cmd, 0, sizeof(cmd));

    result = parse_command_line(input, &cmd);

    check(result == 0, name);

    if (result != 0) {
        return;
    }

    check(cmd.id == expected_id, "request id parsed correctly");
    check(cmd.type == expected_type, "command type parsed correctly");
    check(cmd.has_arg == expected_has_arg, "has_arg parsed correctly");

    if (expected_has_arg) {
        check(cmd.arg == expected_arg, "argument parsed correctly");
    }
}

static void test_invalid_command(const char *input,
                                 const char *name)
{
    parsed_cmd_t cmd;
    int result;

    memset(&cmd, 0, sizeof(cmd));

    result = parse_command_line(input, &cmd);

    check(result != 0, name);
}

int main(void)
{
    printf("=== vmonitor protocol parser tests ===\n\n");

    /*
     * Valid commands
     */

    test_valid_command(
        "1 GET",
        1,
        CMD_GET,
        0,
        0,
        "GET command accepted"
    );

    test_valid_command(
        "2 START",
        2,
        CMD_START,
        0,
        0,
        "START command accepted"
    );

    test_valid_command(
        "3 STOP",
        3,
        CMD_STOP,
        0,
        0,
        "STOP command accepted"
    );

    test_valid_command(
        "4 PERIOD 250",
        4,
        CMD_PERIOD,
        1,
        250,
        "PERIOD command accepted"
    );

    test_valid_command(
        "5 THRESHOLD 35000",
        5,
        CMD_THRESHOLD,
        1,
        35000,
        "THRESHOLD command accepted"
    );

    test_valid_command(
        "6 INJECT 42000",
        6,
        CMD_INJECT,
        1,
        42000,
        "INJECT command accepted"
    );

    test_valid_command(
        "7 WATCH 1",
        7,
        CMD_WATCH,
        1,
        1,
        "WATCH 1 command accepted"
    );

    test_valid_command(
        "8 WATCH 0",
        8,
        CMD_WATCH,
        1,
        0,
        "WATCH 0 command accepted"
    );

    /*
     * Invalid commands
     */

    printf("\n=== Invalid command tests ===\n\n");

    test_invalid_command(
        "",
        "empty input rejected"
    );

    test_invalid_command(
        "GET",
        "missing request id rejected"
    );

    test_invalid_command(
        "-1 GET",
        "negative request id rejected"
    );

    test_invalid_command(
        "abc GET",
        "non-numeric request id rejected"
    );

    test_invalid_command(
        "999999999999999999999 GET",
        "out-of-range request id rejected"
    );

    test_invalid_command(
        "9 UNKNOWN",
        "unknown command rejected"
    );

    test_invalid_command(
        "10 PERIOD",
        "PERIOD without argument rejected"
    );

    test_invalid_command(
        "11 PERIOD 0",
        "PERIOD 0 rejected"
    );

    test_invalid_command(
        "12 PERIOD -1",
        "negative PERIOD rejected"
    );

    test_invalid_command(
        "13 PERIOD 250 extra",
        "PERIOD with extra argument rejected"
    );

    test_invalid_command(
        "14 START extra",
        "START with extra argument rejected"
    );

    test_invalid_command(
        "15 GET extra",
        "GET with extra argument rejected"
    );

    test_invalid_command(
        "16 WATCH",
        "WATCH without argument rejected"
    );

    test_invalid_command(
        "17 WATCH 2",
        "WATCH value other than 0 or 1 rejected"
    );

    test_invalid_command(
        "18 WATCH -1",
        "negative WATCH value rejected"
    );

    test_invalid_command(
        "19 WATCH 1 extra",
        "WATCH with extra argument rejected"
    );

    printf("\n=== Test summary ===\n");

    if (failures == 0) {
        printf("[PASS] All protocol parser tests passed\n");
        return 0;
    }

    printf("[FAIL] %d protocol parser test(s) failed\n", failures);

    return 1;
}