#include <pebble.h>
#include <math.h>
#include "dialog_choice_window.h"
#include "xadow.h"

static Window *s_main_window;
static Layer *s_window_layer, *s_dots_layer, *s_progress_layer, *s_average_layer;
static TextLayer *s_time_layer, *s_step_layer;
static TextLayer *s_conn_status_layer;
static TextLayer *s_data_layer;
static StatusBarLayer *s_status_bar;

struct endpoint
{
	SmartstrapAttribute *attr;
	bool                 available;
}readable_end_points[20];

static int num_endpoints = 0;
static int cur_endpoint = 0;

static SmartstrapAttribute *s_raw_attribute;
static SmartstrapAttribute *s_attr_bat_chg;
static SmartstrapAttribute *s_attr_nfc_uid;

static int cnt_dot = 0;
static int cnt_fail = 0;

static uint8_t s_buffer[256];
static char connection_text[20];
static uint8_t connected = 0;
static uint8_t read_req_pending = 0;
AppTimer *p_timer;

//gps data
static uint16_t vbat, speed, alt;
static int32_t lat, lon;
static uint8_t fix, sat;
static char str_vbat[16];
static char str_lat[16];
static char str_lon[16];
static char str_speed[16];
static char str_alt[16];

//nfc data
static uint8_t nfc_valid_tagid = 0;
static char tagid[16];

static void check_connection(void *context);
static void prv_send_read_request(void *context);
static void connection_status_text_show();
static void connection_status_text_hide();
static void data_text_show();
static void data_text_hide();
void format_number(int32_t input, int input_precision, char *output, int output_precision);

// health
static char s_current_steps_buffer[16];
static int s_step_count = 0, s_step_goal = 0, s_step_average = 0;

// time data
static char s_current_time_buffer[8];

// color information
GColor color_loser;
GColor color_winner;

// is health step data available
static bool step_data_is_available() {
	return HealthServiceAccessibilityMaskAvailable &
		health_service_metric_accessible(HealthMetricStepCount,
			time_start_of_today(), time(NULL));
}

// Daily step goal
static void get_step_goal() {
	const time_t start = time_start_of_today();
	const time_t end = start + SECONDS_PER_DAY;
	s_step_goal = (int)health_service_sum_averaged(
		HealthMetricStepCount, start, end, HealthServiceTimeScopeDaily);
}

static void get_step_count() {
	s_step_count = (int)health_service_sum_today(HealthMetricStepCount);
}

static void get_step_average() {
	const time_t start = time_start_of_today();
	const time_t end = time(NULL);
	s_step_average = (int)health_service_sum_averaged(HealthMetricStepCount,
		start, end, HealthServiceTimeScopeDaily);
}

static void display_step_count() {
	int thousands = s_step_count / 1000;
	int hundreds = s_step_count % 1000;

	if (thousands > 0) {
		snprintf(s_current_steps_buffer, sizeof(s_current_steps_buffer),
			"%d,%03d", thousands, hundreds);
	}
	else {
		snprintf(s_current_steps_buffer, sizeof(s_current_steps_buffer),
			"%d", hundreds);
	}

	text_layer_set_text(s_step_layer, s_current_steps_buffer);
}

static void health_handler(HealthEventType event, void *context) {
	if (event == HealthEventSignificantUpdate) {
		get_step_goal();
	}

	if (event != HealthEventSleepUpdate) {
		get_step_count();
		get_step_average();
		display_step_count();
		layer_mark_dirty(s_progress_layer);
		layer_mark_dirty(s_average_layer);
	}
}

static void tick_handler(struct tm *tick_time, TimeUnits changed) {
	strftime(s_current_time_buffer, sizeof(s_current_time_buffer),
		clock_is_24h_style() ? "%H:%M" : "%l:%M", tick_time);

	text_layer_set_text(s_time_layer, s_current_time_buffer);
}

static void dots_layer_update_proc(Layer *layer, GContext *ctx) {
	const GRect inset = grect_inset(layer_get_bounds(layer), GEdgeInsets(6));

	const int num_dots = 12;
	for (int i = 0; i < num_dots; i++) {
		GPoint pos = gpoint_from_polar(inset, GOvalScaleModeFitCircle,
			DEG_TO_TRIGANGLE(i * 360 / num_dots));
		graphics_context_set_fill_color(ctx, GColorDarkGray);
		graphics_fill_circle(ctx, pos, 2);
	}
}

static void progress_layer_update_proc(Layer *layer, GContext *ctx) {
	const GRect inset = grect_inset(layer_get_bounds(layer), GEdgeInsets(2));

	graphics_context_set_fill_color(ctx,
		s_step_count >= s_step_average ? color_winner : color_loser);

	graphics_fill_radial(ctx, inset, GOvalScaleModeFitCircle, 12,
		DEG_TO_TRIGANGLE(0),
		DEG_TO_TRIGANGLE(360 * (s_step_count / s_step_goal)));
}

static void average_layer_update_proc(Layer *layer, GContext *ctx) {
	if (s_step_average < 1) {
		return;
	}

	const GRect inset = grect_inset(layer_get_bounds(layer), GEdgeInsets(2));
	graphics_context_set_fill_color(ctx, GColorYellow);

	int trigangle = DEG_TO_TRIGANGLE(360 * s_step_average / s_step_goal);
	int line_width_trigangle = 1000;
	// draw a very narrow radial (it's just a line)
	graphics_fill_radial(ctx, inset, GOvalScaleModeFitCircle, 12,
		trigangle - line_width_trigangle, trigangle);
}

static char* smartstrap_result_to_string(SmartstrapResult result) {
	switch (result) {
	case SmartstrapResultOk:
		return "SmartstrapResultOk";
	case SmartstrapResultInvalidArgs:
		return "SmartstrapResultInvalidArgs";
	case SmartstrapResultNotPresent:
		return "SmartstrapResultNotPresent";
	case SmartstrapResultBusy:
		return "SmartstrapResultBusy";
	case SmartstrapResultServiceUnavailable:
		return "SmartstrapResultServiceUnavailable";
	case SmartstrapResultAttributeUnsupported:
		return "SmartstrapResultAttributeUnsupported";
	case SmartstrapResultTimeOut:
		return "SmartstrapResultTimeOut";
	default:
		return "Not a SmartstrapResult value!";
	}
}

static void update_connection_status_text(void)
{
	if (connected) {
		memcpy(connection_text, "Connected", 9);
		connection_text[9] = '\0';
		text_layer_set_text(s_conn_status_layer, connection_text);
		return;
	}

	if (++cnt_dot > 3) {
		cnt_dot = 1;
	}

	memcpy(connection_text, "Connecting\n...", 11 + cnt_dot);
	connection_text[11 + cnt_dot] = '\0';
	text_layer_set_text(s_conn_status_layer, connection_text);
}

static void update_data_text(void)
{
	if (!connected) {
		return;
	}

	snprintf((char *)s_buffer, sizeof(s_buffer), "VBAT: %s\nGPS:\nlat: %s lon: %s\nvel: %s alt: %s\nfix: %d sat. in view: %d\n\nNFC TAG ID:\n %02X %02X %02X %02X", str_vbat, str_lat, str_lon, str_speed, str_alt, fix, sat, tagid[0], tagid[1], tagid[2], tagid[3]);
	text_layer_set_text(s_data_layer, (const char *)s_buffer);
}

static void prv_did_read(SmartstrapAttribute *attr, SmartstrapResult result,
	const uint8_t *data, size_t length)
{
	uint16_t service_id = smartstrap_attribute_get_service_id(attr);
	uint16_t attr_id = smartstrap_attribute_get_attribute_id(attr);
	APP_LOG(APP_LOG_LEVEL_DEBUG, "did_read(%04x, %04x, %s)", service_id, attr_id, smartstrap_result_to_string(result));

	read_req_pending = 0;
	app_timer_cancel(p_timer);

	if (service_id == SERVICE_BAT && attr_id == ATTR_BAT_V && length >= 2)
	{
		//the returned value is uint16_t,  it's 100 * volt
		memcpy(&vbat, data, 2);
		//APP_LOG(APP_LOG_LEVEL_DEBUG, "vbat: %d", vbat);
		format_number(vbat, 2, str_vbat, 1);
	}
	else if (service_id == SERVICE_GPS && attr_id == ATTR_GPS_LOCATION && length >= 8)
	{
		//sint32[2],The current longitude and latitude in degrees with a precision of 1/10^7.
		//The latitude comes before the longitude in the data.
		//For example, Pebble HQ is at (37.4400662, -122.1583808), which would be specified as {374400662, -1221583808}.
		memcpy(&lat, data, 4);
		memcpy(&lon, data + 4, 4);
		format_number(lat, 7, str_lat, 4);
		format_number(lon, 7, str_lon, 4);
	}
	else if (service_id == SERVICE_GPS && attr_id == ATTR_GPS_SPEED && length >= 2)
	{
		//the returned value is uint16_t
		//The current speed in meters per second with a precision of 1/100. For example, 1.5 m/s would be specified as 150.
		memcpy(&speed, data, 2);
		format_number(speed, 2, str_speed, 2);
	}
	else if (service_id == SERVICE_GPS && attr_id == ATTR_GPS_ALTITUDE && length >= 2)
	{
		//the returned value is uint16_t
		//The current altitude in meters with a precision of 1/100. For example, 1.5 m would be specified as 150.
		memcpy(&alt, data, 2);
		format_number(alt, 2, str_alt, 2);
	}
	else if (service_id == SERVICE_GPS && attr_id == ATTR_GPS_FIX_QUALITY && length >= 1)
	{
		//the returned value is uint8_t
		//http://www.gpsinformation.org/dale/nmea.htm#GGA
		memcpy(&fix, data, 1);
	}
	else if (service_id == SERVICE_GPS && attr_id == ATTR_GPS_SATELLITES && length >= 1)
	{
		//the returned value is uint8_t
		//The number of GPS satellites (typically reported via NMEA.
		memcpy(&sat, data, 1);
	}
	else if (service_id == SERVICE_NFC && attr_id == ATTR_NFC_GET_UID)
	{
		//the returned data is an array of uint8_t with length indicated
		if (length > 0)
		{
			length = min(length, 16);
			memcpy(tagid, data, length);
		}
		else
		{
			memset(tagid, 0, sizeof(tagid));
		}
	}

	app_timer_register(200, prv_send_read_request, NULL);
	update_data_text();
}

static void read_request_timeout(void *context)
{
	read_req_pending = 0;
	app_timer_register(100, prv_send_read_request, NULL);
}

static void prv_did_write(SmartstrapAttribute *attr, SmartstrapResult result) {
	uint16_t service_id = smartstrap_attribute_get_service_id(attr);
	uint16_t attr_id = smartstrap_attribute_get_attribute_id(attr);
	APP_LOG(APP_LOG_LEVEL_DEBUG, "did_write(%04x, %04x, %s)", service_id, attr_id, smartstrap_result_to_string(result));

	if (service_id == SERVICE_BAT && attr_id == ATTR_BAT_CHG)
	{
		dialog_choice_window_pop();
	}
}

static void prv_send_read_request(void *context) {
	if (!connected)
	{
		app_timer_register(1, check_connection, NULL);
		return;
	}

	if (read_req_pending)
	{
		return;
	}

	struct endpoint *ep;
	SmartstrapAttribute *attr;

	if (nfc_valid_tagid)
	{
		attr = s_attr_nfc_uid;
	}
	else
	{
		int cnt = 0;
		do
		{
			ep = &readable_end_points[cur_endpoint];
			if (cnt++ > num_endpoints)
			{
				app_timer_register(100, check_connection, NULL);
				return;
			}
			uint16_t service_id = smartstrap_attribute_get_service_id(ep->attr);
			uint16_t attr_id = smartstrap_attribute_get_attribute_id(ep->attr);
			if (ep->available && !smartstrap_service_is_available(service_id))
			{
				APP_LOG(APP_LOG_LEVEL_DEBUG, "%04x %04x is not available", service_id, attr_id);

				ep->available = false;
			}
			if (ep->available)
			{
				attr = ep->attr;
				break;
			}
			else
			{
				if (++cur_endpoint >= num_endpoints) cur_endpoint = 0;
			}
		} while (1);
	}


	SmartstrapResult result = smartstrap_attribute_read(attr);
	if (result != SmartstrapResultOk)
	{
		APP_LOG(APP_LOG_LEVEL_ERROR, "Read of %04x failed with result: %s", smartstrap_attribute_get_attribute_id(attr), smartstrap_result_to_string(result));
		if (result == SmartstrapResultBusy)
		{
			app_timer_register(100, prv_send_read_request, NULL);
		}
		else if (result == SmartstrapResultTimeOut)
		{
			app_timer_register(100, check_connection, NULL);
		}
		else
		{
			app_timer_register(1000, prv_send_read_request, NULL);
		}
	}
	else
	{
		if (attr == s_attr_nfc_uid)
		{
			nfc_valid_tagid = 0;
		}
		else
		{
			if (++cur_endpoint >= num_endpoints) cur_endpoint = 0;
		}
		read_req_pending = 1;
		p_timer = app_timer_register(1000, read_request_timeout, NULL);
	}
}

static void prv_availablility_status_changed(SmartstrapServiceId service_id, bool is_available) {
	APP_LOG(APP_LOG_LEVEL_DEBUG, "Availability for 0x%x is %d", service_id, is_available);

	if (service_id == SMARTSTRAP_RAW_DATA_SERVICE_ID && !is_available)
	{
		read_req_pending = 0;
		app_timer_register(1000, check_connection, NULL);
	}
	if (is_available)
	{
		for (int i = 0; i < num_endpoints; i++)
		{
			if (smartstrap_attribute_get_service_id(readable_end_points[i].attr) == service_id)
			{
				readable_end_points[i].available = true;
			}
		}
	}
}

static void prv_notified(SmartstrapAttribute *attr) {
	uint16_t service_id = smartstrap_attribute_get_service_id(attr);
	uint16_t attr_id = smartstrap_attribute_get_attribute_id(attr);

	APP_LOG(APP_LOG_LEVEL_DEBUG, "notified(%04x, %04x)", service_id, attr_id);

	if (service_id == SERVICE_NFC && attr_id == ATTR_NFC_GET_UID)
	{
		nfc_valid_tagid = 1;
	}
}

static void check_connection(void *context) {
	if (smartstrap_service_is_available(SMARTSTRAP_RAW_DATA_SERVICE_ID)) {
		connected = 1;
		APP_LOG(APP_LOG_LEVEL_DEBUG, "connection ok");
		update_connection_status_text();
		connection_status_text_hide();
		data_text_show();
		app_timer_register(100, prv_send_read_request, NULL);
	}
	else {
		connected = 0;
		APP_LOG(APP_LOG_LEVEL_DEBUG, "connecting...");
		data_text_hide();
		connection_status_text_show();
		update_connection_status_text();
		app_timer_register(1000, check_connection, NULL);
	}
}

static void connection_status_text_show()
{
	layer_set_hidden(text_layer_get_layer(s_conn_status_layer), false);
}

static void connection_status_text_hide()
{
	layer_set_hidden(text_layer_get_layer(s_conn_status_layer), true);
}

static void data_text_show()
{
	layer_set_hidden(text_layer_get_layer(s_data_layer), false);
	update_data_text();
}

static void data_text_hide()
{
	layer_set_hidden(text_layer_get_layer(s_data_layer), true);
}

static void prv_main_window_load(Window *window) {

	GRect window_bounds = layer_get_bounds(s_window_layer);

	s_status_bar = status_bar_layer_create();
	status_bar_layer_set_separator_mode(s_status_bar, StatusBarLayerSeparatorModeDotted);
	status_bar_layer_set_colors(s_status_bar, GColorDarkGreen, GColorWhite);
	layer_add_child(s_window_layer, status_bar_layer_get_layer(s_status_bar));

	s_conn_status_layer = text_layer_create(GRect(0, 56, 144, 80));
	text_layer_set_font(s_conn_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28));
	update_connection_status_text();
	text_layer_set_text_color(s_conn_status_layer, GColorDarkGreen);
	text_layer_set_background_color(s_conn_status_layer, GColorClear);
	text_layer_set_text_alignment(s_conn_status_layer, GTextAlignmentCenter);
	text_layer_set_overflow_mode(s_conn_status_layer, GTextOverflowModeWordWrap);
	layer_add_child(s_window_layer, text_layer_get_layer(s_conn_status_layer));

	s_data_layer = text_layer_create(GRect(0, 10, 144, 160));
	text_layer_set_font(s_data_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
	text_layer_set_text_color(s_data_layer, GColorBlack);
	text_layer_set_background_color(s_data_layer, GColorClear);
	text_layer_set_text_alignment(s_data_layer, GTextAlignmentLeft);
	text_layer_set_overflow_mode(s_data_layer, GTextOverflowModeWordWrap);
	layer_add_child(s_window_layer, text_layer_get_layer(s_data_layer));

	data_text_hide();
	
	// Dots for the progress indicator
	s_dots_layer = layer_create(window_bounds);
	layer_set_update_proc(s_dots_layer, dots_layer_update_proc);
	layer_add_child(s_window_layer, s_dots_layer);

	// Progress indicator
	s_progress_layer = layer_create(window_bounds);
	layer_set_update_proc(s_progress_layer, progress_layer_update_proc);
	layer_add_child(s_window_layer, s_progress_layer);

	// Average indicator
	s_average_layer = layer_create(window_bounds);
	layer_set_update_proc(s_average_layer, average_layer_update_proc);
	layer_add_child(s_window_layer, s_average_layer);

	// Create a layer to hold the current time
	s_time_layer = text_layer_create(
		GRect(0, PBL_IF_ROUND_ELSE(82, 78), window_bounds.size.w, 38));
	text_layer_set_text_color(s_time_layer, GColorWhite);
	text_layer_set_background_color(s_time_layer, GColorClear);
	text_layer_set_font(s_time_layer,
		fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK));
	text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
	layer_add_child(s_window_layer, text_layer_get_layer(s_time_layer));

	// Create a layer to hold the current step count
	s_step_layer = text_layer_create(
		GRect(0, PBL_IF_ROUND_ELSE(58, 54), window_bounds.size.w, 38));
	text_layer_set_background_color(s_step_layer, GColorClear);
	text_layer_set_font(s_step_layer,
		fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
	text_layer_set_text_alignment(s_step_layer, GTextAlignmentCenter);
	layer_add_child(s_window_layer, text_layer_get_layer(s_step_layer));

	// Subscribe to health events if we can
	if (step_data_is_available()) {
		health_service_events_subscribe(health_handler, NULL);
	}

}

static void prv_main_window_unload(Window *window) {
	text_layer_destroy(s_conn_status_layer);
	text_layer_destroy(s_data_layer);
	layer_destroy(text_layer_get_layer(s_time_layer));
	layer_destroy(text_layer_get_layer(s_step_layer));
	layer_destroy(s_dots_layer);
	layer_destroy(s_progress_layer);
	layer_destroy(s_average_layer);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
	//uint8_t *data = context;
	//ask_for_scroll(data, ScrollDirectionUp);
	if (connected)
	{
		connection_status_text_hide();
		data_text_show();
	}
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
	//WeatherAppData *data = context;
	//ask_for_scroll(data, ScrollDirectionDown);
	//connected = 0;
	data_text_hide();
	connection_status_text_show();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
	if (connected) {
		dialog_choice_window_push(s_attr_bat_chg);
	}
}

static void click_config_provider(void *context) {
	window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
	window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
	window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
}

static void prv_init(void) {
	color_loser = GColorPictonBlue;
	color_winner = GColorJaegerGreen;

	s_main_window = window_create();
	s_window_layer = window_get_root_layer(s_main_window);

	//window_set_click_config_provider_with_context(s_main_window, click_config_provider, NULL);
	window_set_window_handlers(s_main_window, (WindowHandlers) {
		.load = prv_main_window_load,
		.unload = prv_main_window_unload
	});
	window_stack_push(s_main_window, true);

	SmartstrapHandlers handlers = (SmartstrapHandlers) {
		.availability_did_change = prv_availablility_status_changed,
		.did_write = prv_did_write,
		.did_read = prv_did_read,
		.notified = prv_notified
	};
	smartstrap_subscribe(handlers);
	smartstrap_set_timeout(500);

	//read/write attrib - raw data service
	s_raw_attribute = smartstrap_attribute_create(0, 0, 100);

	//write attrib - enable or disable the strap charging pebble time
	s_attr_bat_chg = smartstrap_attribute_create(0x2003, 0x1002, 4);

	s_attr_nfc_uid = smartstrap_attribute_create(SERVICE_NFC, ATTR_NFC_GET_UID, 10);

	//readable attrib - get the voltage of the battery of strap
	readable_end_points[num_endpoints].attr = smartstrap_attribute_create(SERVICE_BAT, ATTR_BAT_V, 4);
	readable_end_points[num_endpoints++].available = true;

	readable_end_points[num_endpoints].attr = smartstrap_attribute_create(SERVICE_GPS, ATTR_GPS_LOCATION, 16);
	readable_end_points[num_endpoints++].available = true;

	readable_end_points[num_endpoints].attr = smartstrap_attribute_create(SERVICE_GPS, ATTR_GPS_SPEED, 4);
	readable_end_points[num_endpoints++].available = true;

	readable_end_points[num_endpoints].attr = smartstrap_attribute_create(SERVICE_GPS, ATTR_GPS_ALTITUDE, 4);
	readable_end_points[num_endpoints++].available = true;

	readable_end_points[num_endpoints].attr = smartstrap_attribute_create(SERVICE_GPS, ATTR_GPS_FIX_QUALITY, 2);
	readable_end_points[num_endpoints++].available = true;

	readable_end_points[num_endpoints].attr = smartstrap_attribute_create(SERVICE_GPS, ATTR_GPS_SATELLITES, 2);
	readable_end_points[num_endpoints++].available = true;

	app_timer_register(1000, check_connection, NULL);
	tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void prv_deinit(void) {
	window_destroy(s_main_window);
	smartstrap_unsubscribe();
}

int main(void) {
	prv_init();
	app_event_loop();
	prv_deinit();
}

uint32_t upow(int p)
{
	uint32_t ret = 1;
	for (int i = 0; i < p; i++)
	{
		ret *= 10;
	}
	return ret;
}

void format_number(int32_t input, int input_precision, char *output, int output_precision)
{
	if (input < 0)
	{
		*(output++) = '-';
		input = -input;
	}
	uint32_t round = input / upow(input_precision);
	uint32_t fraction = input % upow(input_precision);

	if (output_precision >= input_precision)
	{
		fraction = fraction * upow((output_precision - input_precision));
	}
	else
	{
		fraction = fraction / upow((input_precision - output_precision));
		if (fraction % upow(input_precision - output_precision) >= upow(input_precision - output_precision) / 2)
		{
			fraction += 1;
			round += fraction / upow(output_precision);
			fraction = fraction % upow(output_precision);
		}
	}

	snprintf(output, 6, "%lu", round);
	output += strlen(output);

	if (output_precision > 0)
	{
		*(output++) = '.';
		snprintf(output, 6, "%lu", fraction);
		output += strlen(output);
	}
	*output = '\0';
}