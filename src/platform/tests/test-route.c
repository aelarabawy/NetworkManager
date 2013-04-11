#include "test-common.h"

#define DEVICE_NAME "nm-test-device"

static void
ip4_route_callback (NMPlatform *platform, NMPlatformIP4Route *received, SignalData *data)
{
	g_assert (received);

	if (data->ifindex && data->ifindex != received->ifindex)
		return;

	if (data->loop)
		g_main_loop_quit (data->loop);

	if (data->received)
		g_error ("Received signal '%s' a second time.", data->name);

	data->received = TRUE;
}

static void
ip6_route_callback (NMPlatform *platform, NMPlatformIP6Route *received, SignalData *data)
{
	g_assert (received);

	if (data->ifindex && data->ifindex != received->ifindex)
		return;

	if (data->loop)
		g_main_loop_quit (data->loop);

	if (data->received)
		g_error ("Received signal '%s' a second time.", data->name);

	data->received = TRUE;
}

static void
test_ip4_route ()
{
	SignalData *route_added = add_signal (NM_PLATFORM_IP4_ROUTE_ADDED, ip4_route_callback);
	SignalData *route_removed = add_signal (NM_PLATFORM_IP4_ROUTE_REMOVED, ip4_route_callback);
	int ifindex = nm_platform_link_get_ifindex (DEVICE_NAME);
	GArray *routes;
	NMPlatformIP4Route rts[3];
	in_addr_t network;
	int plen = 24;
	in_addr_t gateway;
	int metric = 20;
	int mss = 1000;

	inet_pton (AF_INET, "192.0.2.0", &network);
	inet_pton (AF_INET, "198.51.100.0", &gateway);

	/* Add route to gateway */
	g_assert (nm_platform_ip4_route_add (ifindex, gateway, 32, INADDR_ANY, metric, 0)); no_error ();
	accept_signal (route_added);

	/* Add route */
	g_assert (!nm_platform_ip4_route_exists (ifindex, network, plen, gateway, metric)); no_error ();
	g_assert (nm_platform_ip4_route_add (ifindex, network, plen, gateway, metric, mss)); no_error ();
	g_assert (nm_platform_ip4_route_exists (ifindex, network, plen, gateway, metric)); no_error ();
	accept_signal (route_added);

	/* Add route again */
	g_assert (!nm_platform_ip4_route_add (ifindex, network, plen, gateway, metric, mss));
	error (NM_PLATFORM_ERROR_EXISTS);

	/* Test route listing */
	routes = nm_platform_ip4_route_get_all (ifindex);
	memset (rts, 0, sizeof (rts));
	rts[0].network = gateway;
	rts[0].plen = 32;
	rts[0].ifindex = ifindex;
	rts[0].gateway = INADDR_ANY;
	rts[0].metric = metric;
	rts[1].network = network;
	rts[1].plen = plen;
	rts[1].ifindex = ifindex;
	rts[1].gateway = gateway;
	rts[1].metric = metric;
	g_assert (routes->len == 2);
	g_assert (!memcmp (routes->data, rts, sizeof (rts)));
	g_array_unref (routes);

	/* Remove route */
	g_assert (nm_platform_ip4_route_delete (ifindex, network, plen, gateway, metric)); no_error ();
	g_assert (!nm_platform_ip4_route_exists (ifindex, network, plen, gateway, metric));
	accept_signal (route_removed);

	/* Remove route again */
	g_assert (!nm_platform_ip4_route_delete (ifindex, network, plen, gateway, metric));
	error (NM_PLATFORM_ERROR_NOT_FOUND);

	free_signal (route_added);
	free_signal (route_removed);
}

static void
test_ip6_route ()
{
	SignalData *route_added = add_signal (NM_PLATFORM_IP6_ROUTE_ADDED, ip6_route_callback);
	SignalData *route_removed = add_signal (NM_PLATFORM_IP6_ROUTE_REMOVED, ip6_route_callback);
	int ifindex = nm_platform_link_get_ifindex (DEVICE_NAME);
	GArray *routes;
	NMPlatformIP6Route rts[3];
	struct in6_addr network;
	int plen = 64;
	struct in6_addr gateway;
	int metric = 20;
	int mss = 1000;

	inet_pton (AF_INET6, "2001:db8:a:b:0:0:0:0", &network);
	inet_pton (AF_INET6, "2001:db8:c:d:1:2:3:4", &gateway);

	/* Add gateway address */
	g_assert (nm_platform_ip6_route_add (ifindex, gateway, 128, in6addr_any, metric, 0)); no_error ();
	accept_signal (route_added);

	/* Add route */
	g_assert (!nm_platform_ip6_route_exists (ifindex, network, plen, gateway, metric)); no_error ();
	g_assert (nm_platform_ip6_route_add (ifindex, network, plen, gateway, metric, mss)); no_error ();
	g_assert (nm_platform_ip6_route_exists (ifindex, network, plen, gateway, metric)); no_error ();
	accept_signal (route_added);

	/* Add route again */
	g_assert (!nm_platform_ip6_route_add (ifindex, network, plen, gateway, metric, mss));
	error (NM_PLATFORM_ERROR_EXISTS);

	/* Test route listing */
	routes = nm_platform_ip6_route_get_all (ifindex);
	memset (rts, 0, sizeof (rts));
	rts[0].network = gateway;
	rts[0].plen = 128;
	rts[0].ifindex = ifindex;
	rts[0].gateway = in6addr_any;
	rts[0].metric = metric;
	rts[1].network = network;
	rts[1].plen = plen;
	rts[1].ifindex = ifindex;
	rts[1].gateway = gateway;
	rts[1].metric = metric;
	g_assert (routes->len == 2);
	g_assert (!memcmp (routes->data, rts, sizeof (rts)));
	g_array_unref (routes);

	/* Remove route */
	g_assert (nm_platform_ip6_route_delete (ifindex, network, plen, gateway, metric)); no_error ();
	g_assert (!nm_platform_ip6_route_exists (ifindex, network, plen, gateway, metric));
	accept_signal (route_removed);

	/* Remove route again */
	g_assert (!nm_platform_ip6_route_delete (ifindex, network, plen, gateway, metric));
	error (NM_PLATFORM_ERROR_NOT_FOUND);

	free_signal (route_added);
	free_signal (route_removed);
}

int
main (int argc, char **argv)
{
	int result;

	openlog (G_LOG_DOMAIN, LOG_CONS | LOG_PERROR, LOG_DAEMON);
	g_type_init ();
	g_test_init (&argc, &argv, NULL);
	/* Enable debug messages if called with --debug */
	for (; *argv; argv++)
		if (!g_strcmp0 (*argv, "--debug"))
			nm_logging_setup ("debug", NULL, NULL);

	SETUP ();

	/* Clean up */
	nm_platform_link_delete_by_name (DEVICE_NAME);
	g_assert (!nm_platform_link_exists (DEVICE_NAME));
	g_assert (nm_platform_dummy_add (DEVICE_NAME));
	g_assert (nm_platform_link_set_up (nm_platform_link_get_ifindex (DEVICE_NAME)));

	g_test_add_func ("/route/ip4", test_ip4_route);
	g_test_add_func ("/route/ip6", test_ip6_route);

	result = g_test_run ();

	nm_platform_link_delete_by_name (DEVICE_NAME);
	nm_platform_free ();
	return result;
}