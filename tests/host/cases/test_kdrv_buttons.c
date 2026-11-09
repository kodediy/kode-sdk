/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_buttons.c
 * @brief kdrv_buttons: six switches, one sample, and the pin that is its own.
 */
#include "unity.h"

#include "Mockkhal_gpio.h"

#include "kbsp.h"
#include "kdrv_buttons.h"

#define BIT(b) ((uint8_t)(1U << (unsigned)(b)))

TEST_CASE("nothing pressed decodes to nothing", "[kdrv_buttons]")
{
    TEST_ASSERT_EQUAL_HEX8(0x00U, kdrv_buttons_decode(0xFFU, false));
}

/* Each expander pin to its button, low meaning pressed. The mapping is the
 * board's (kbsp), repeated here as literals so a swapped constant is caught. */
TEST_CASE("each expander pin decodes to its own button, low meaning pressed",
          "[kdrv_buttons]")
{
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_BOT),   kdrv_buttons_decode(0xFEU, false)); /* P0 */
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_UP),    kdrv_buttons_decode(0xFDU, false)); /* P1 */
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_LEFT),  kdrv_buttons_decode(0xFBU, false)); /* P2 */
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_DOWN),  kdrv_buttons_decode(0xF7U, false)); /* P3 */
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_RIGHT), kdrv_buttons_decode(0xEFU, false)); /* P4 */
}

/* P5, P6 and P7 are the gauge's interrupt, the touch's and the panel's
 * supply. Whatever they read, they are not buttons. */
TEST_CASE("the three pins that are not buttons decode to nothing", "[kdrv_buttons]")
{
    TEST_ASSERT_EQUAL_HEX8(0x00U, kdrv_buttons_decode(0x1FU, false));
}

TEST_CASE("the top button comes from its own pin, and a diagonal is two bits",
          "[kdrv_buttons]")
{
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_TOP), kdrv_buttons_decode(0xFFU, true));
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_UP) | BIT(KDRV_BTN_LEFT),
                           kdrv_buttons_decode(0xF9U, false));
    TEST_ASSERT_EQUAL_HEX8(0x3FU, kdrv_buttons_decode(0x00U, true));
}

TEST_CASE("every button has a name, and an unknown one is a question mark",
          "[kdrv_buttons]")
{
    TEST_ASSERT_EQUAL_STRING("pad-up",    kdrv_button_name(KDRV_BTN_UP));
    TEST_ASSERT_EQUAL_STRING("pad-down",  kdrv_button_name(KDRV_BTN_DOWN));
    TEST_ASSERT_EQUAL_STRING("pad-left",  kdrv_button_name(KDRV_BTN_LEFT));
    TEST_ASSERT_EQUAL_STRING("pad-right", kdrv_button_name(KDRV_BTN_RIGHT));
    TEST_ASSERT_EQUAL_STRING("top",       kdrv_button_name(KDRV_BTN_TOP));
    TEST_ASSERT_EQUAL_STRING("bottom",    kdrv_button_name(KDRV_BTN_BOT));
    TEST_ASSERT_EQUAL_STRING("?",         kdrv_button_name(KDRV_BTN_COUNT));
}

/* What the owner's edge saw. */
static unsigned s_edges;
static void    *s_edge_arg;

static void on_edge(void *arg)
{
    ++s_edges;
    s_edge_arg = arg;
}

/* What the driver attached to the pin: its own handler, which withdraws the
 * wake and then calls the owner. */
static khal_gpio_cb_t s_isr;
static void          *s_isr_arg;

static kode_status_t capture_attach(uint8_t pin, khal_gpio_irq_t kind,
                                    khal_gpio_cb_t cb, void *arg, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_UINT8(KBSP_PIN_BUTTON_TOP, pin);
    TEST_ASSERT_EQUAL_INT(KHAL_GPIO_IRQ_ANY_EDGE, kind);
    TEST_ASSERT_NOT_NULL(cb);
    s_isr     = cb;
    s_isr_arg = arg;
    return KODE_OK;
}

/* GPIO35 has R38 on the board; the internal pull-up is belt and braces and
 * costs nothing. The interrupt is any edge: a contact, not a request line. */
TEST_CASE("GPIO35 is an input with a pull-up, an any-edge interrupt, re-armed on request",
          "[kdrv_buttons]")
{
    int arg;

    Mockkhal_gpio_Init();

    khal_gpio_input_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, KHAL_GPIO_PULL_UP, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_buttons_init());

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_buttons_top_attach(NULL, NULL));
    khal_gpio_irq_attach_StubWithCallback(&capture_attach);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_buttons_top_attach(on_edge, &arg));

    /* An edge with no wake armed goes to the owner, with the owner's argument,
     * and touches the pin no further. */
    s_edges    = 0U;
    s_edge_arg = NULL;
    s_isr(s_isr_arg);
    TEST_ASSERT_EQUAL_UINT(1U, s_edges);
    TEST_ASSERT_EQUAL_PTR(&arg, s_edge_arg);

    khal_gpio_irq_enable_Expect(KBSP_PIN_BUTTON_TOP);
    kdrv_buttons_top_rearm();

    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, false);
    TEST_ASSERT_TRUE(kdrv_buttons_top_pressed());
    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, true);
    TEST_ASSERT_FALSE(kdrv_buttons_top_pressed());

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The top button wakes the board. The wake is a level and a held button would
 * wake the chip in a loop, so the interrupt withdraws it before the owner hears
 * of the edge, once; AWAKE then has nothing left to take away. */
TEST_CASE("GPIO35 armed as a light-sleep wake is withdrawn by its own edge, once",
          "[kdrv_buttons]")
{
    int arg;

    Mockkhal_gpio_Init();
    khal_gpio_irq_attach_StubWithCallback(&capture_attach);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_buttons_top_attach(on_edge, &arg));
    s_edges = 0U;

    khal_gpio_wake_low_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_buttons_top_sleep_wake(true));

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, false, KODE_OK);
    s_isr(s_isr_arg);
    TEST_ASSERT_EQUAL_UINT(1U, s_edges);
    Mockkhal_gpio_Verify();

    s_isr(s_isr_arg);                           /* the release: nothing to withdraw */
    TEST_ASSERT_EQUAL_UINT(2U, s_edges);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_buttons_top_sleep_wake(false));

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* Woken by anything else, the wake is still armed and AWAKE takes it away. */
TEST_CASE("GPIO35's wake, armed and not used, is taken away on request, once",
          "[kdrv_buttons]")
{
    Mockkhal_gpio_Init();

    khal_gpio_wake_low_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_buttons_top_sleep_wake(true));

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, false, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_buttons_top_sleep_wake(false));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_buttons_top_sleep_wake(false));

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* A wake that would not arm is not believed armed: nothing to take away. */
TEST_CASE("a GPIO35 wake the pin refuses is said, and not believed", "[kdrv_buttons]")
{
    Mockkhal_gpio_Init();

    khal_gpio_wake_low_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_buttons_top_sleep_wake(true));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_buttons_top_sleep_wake(false));

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("a pin that will not configure is an error", "[kdrv_buttons]")
{
    Mockkhal_gpio_Init();
    khal_gpio_input_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, KHAL_GPIO_PULL_UP, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_buttons_init());
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}
