using IppPrinter;
using Microsoft.Extensions.Logging;
using System.Runtime.InteropServices;

var shutdownHandler = new ShutdownHandler();
using var loggerFactory = LoggerFactory.Create(builder =>
{
    builder.AddConsole();
    builder.SetMinimumLevel(LogLevel.Debug);
});

try
{
    using var sigtermRegistration =
        PosixSignalRegistration.Create(PosixSignal.SIGTERM, _ => shutdownHandler.Shutdown());
    using var sigintRegistration = PosixSignalRegistration.Create(PosixSignal.SIGINT, _ => shutdownHandler.Shutdown());

    Console.CancelKeyPress += (_, e) =>
    {
        e.Cancel = true;
        shutdownHandler.Shutdown();
    };

    await new IppGadget(loggerFactory).RunAsync(shutdownHandler.Token);
}
catch (OperationCanceledException)
{
    /* clean shutdown */
}

internal sealed class ShutdownHandler
{
    private readonly CancellationTokenSource _cts = new();

    public CancellationToken Token => _cts.Token;

    public void Shutdown()
    {
        if (!_cts.IsCancellationRequested)
        {
            _cts.Cancel();
        }
    }
}