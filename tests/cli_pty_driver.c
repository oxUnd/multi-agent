/* Exercise the real CLI with an isolated database and a local model server. */
#include "sapi/cli/cli.h"
#include "http/client.h"
#include <locale.h>

extern const char *default_db_path;

int main(int argc, char **argv)
{
	struct cli_context ctx;
	int rc;

	if (argc != 4)
		return 2;
	(void)setlocale(LC_ALL, "");
	default_db_path = argv[3];
	http_init();
	rc = cli_init(&ctx, argv[1], argv[2], NULL, CLI_PRESENT_INTERACTIVE);
	if (rc == 0) {
		cli_run(&ctx);
		cli_shutdown(&ctx);
	}
	http_cleanup();
	return rc == 0 ? 0 : 1;
}
