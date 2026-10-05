/* The same foundation assertions can run on the target. Compiling this
 * firmware alone does not establish runtime or memory/stack qualification. */
int main(void);

void app_main(void)
{
    (void)main();
}
