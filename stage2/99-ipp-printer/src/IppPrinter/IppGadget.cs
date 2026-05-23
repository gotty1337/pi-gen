using Microsoft.Extensions.Logging;
using System.Buffers.Binary;
using System.Text;

namespace IppPrinter;

/// <summary>
/// FunctionFS / Linux USB Printer Gadget daemon.
///
/// Runs entirely as async I/O on three device files:
///   ep0  – control endpoint  (read events / write control-request replies)
///   ep1  – bulk OUT endpoint (read HTTP+IPP requests from host)
///   ep2  – bulk IN  endpoint (write HTTP+IPP responses to host)
/// </summary>
public sealed class IppGadget(ILoggerFactory loggerFactory)
{
    private readonly ILogger _logger = loggerFactory.CreateLogger<IppGadget>();

    // ── configurable ──────────────────────────────────────────────────────────
    private const string FfsMount = "/dev/ffs-ipp0";

    private const string DeviceId =
        "MFG:RaspberryPi;" +
        "MDL:IPP USB Printer;" +
        "CMD:IPP,URF,PWGRaster;" +
        "CLS:PRINTER;" +
        "DES:Raspberry Pi IPP USB Printer;" +
        "URF:V1.4,CP1,W8,SRGB24,OB10;" +
        "IPP Technology:IPP Printing;";

    // ── FunctionFS constants ──────────────────────────────────────────────────
    private const uint FfsMagicDesc = 3;
    private const uint FfsMagicStr = 2;
    private const uint FfsHasFs = 1;
    private const uint FfsHasHs = 2;

    // ep0 event types (byte at offset 8 of a 12-byte usb_functionfs_event)
    private const byte EvBind = 0;
    private const byte EvUnbind = 1;
    private const byte EvEnable = 2;
    private const byte EvDisable = 3;
    private const byte EvSetup = 4;
    private const int EventSize = 12;

    // ── USB descriptor constants ──────────────────────────────────────────────
    private const byte UsbDtInterface = 0x04;
    private const byte UsbDtEndpoint = 0x05;
    private const byte UsbXFerBulk = 0x02;
    private const byte UsbDirIn = 0x80;
    private const byte UsbClsPrinter = 0x07;
    private const byte UsbScPrinter = 0x01;
    private const byte UsbProtoIpp = 0x04; // IPP-over-USB

    // USB Printer-class control requests
    private const byte ReqGetDeviceId = 0x00;
    private const byte ReqGetPortStatus = 0x01;
    private const byte ReqSoftReset = 0x02;

    // ── IPP tag constants ─────────────────────────────────────────────────────
    private const byte TagOperGrp = 0x01;
    private const byte TagJobGrp = 0x02;
    private const byte TagEnd = 0x03;
    private const byte TagPrintGrp = 0x04;
    private const byte TagInteger = 0x21;
    private const byte TagBoolean = 0x22;
    private const byte TagEnum = 0x23;
    private const byte TagText = 0x41; // textWithoutLanguage
    private const byte TagKeyword = 0x44;
    private const byte TagUri = 0x45;
    private const byte TagCharset = 0x47;
    private const byte TagLang = 0x48;
    private const byte TagMime = 0x49;

    /// <summary>
    /// Represents whether the gadget is currently enabled (i.e. enumerated by the host and ready to accept IPP jobs).
    /// </summary>
    private bool _isEnabled;

    private readonly Lock _isEnabledLock = new();

    private bool IsIsEnabled
    {
        get
        {
            lock (_isEnabledLock) return _isEnabled;
        }
        set
        {
            lock (_isEnabledLock)
            {
                _isEnabled = value;
            }
        }
    }

    private int _nextJobId = 1;


    /// <summary>
    /// Run IPP USB gadget main loop until cancellation is requested on the provided token.
    /// </summary>
    /// <exception cref="Exception" />
    public async Task RunAsync(CancellationToken ct)
    {
        _logger.LogInformation("Starting IPP USB gadget …");

        _logger.LogInformation("Opening ep0 …");

        // bufferSize:1 → minimal buffering; WriteThrough → no kernel write cache
        await using var ep0 = new FileStream($"{FfsMount}/ep0", FileMode.Open, FileAccess.ReadWrite, FileShare.None,
            bufferSize: 1, FileOptions.WriteThrough);

        _logger.LogInformation("Writing USB descriptors …");

        await ep0.WriteAsync(BuildDescriptors(), ct);
        await ep0.FlushAsync(ct);

        await ep0.WriteAsync(BuildStrings(), ct);
        await ep0.FlushAsync(ct);

        _logger.LogInformation("Descriptors written — waiting for gadget bind.");

        // Bulk-endpoint I/O runs on a background task
        var ippTask = Task.Run(() => BulkLoopAsync(ct), ct);

        // ep0 event loop (runs on this task)
        var evBuf = new byte[EventSize];
        while (!ct.IsCancellationRequested)
        {
            int n;
            try
            {
                n = await ep0.ReadAsync(evBuf, ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (IOException ex)
            {
                _logger.LogError("ep0 read: {Message}", ex.Message);
                break;
            }

            if (n < EventSize) continue;

            byte evType = evBuf[8];
            switch (evType)
            {
                case EvBind:
                    _logger.LogInformation("Gadget bound to UDC");
                    break;
                case EvUnbind:
                    _logger.LogInformation("Gadget unbound");
                    IsIsEnabled = false;
                    break;
                case EvEnable:
                    _logger.LogInformation("Gadget enabled by host (enumerated)");
                    IsIsEnabled = true;
                    break;
                case EvDisable:
                    _logger.LogInformation("Gadget disabled by host");
                    IsIsEnabled = false;
                    break;
                case EvSetup:
                    await HandleSetupAsync(ep0, evBuf, ct);
                    break;
                default:
                    _logger.LogDebug("Unknown ep0 event 0x{evType:X2}", evType);
                    break;
            }
        }

        _logger.LogInformation("Stopping …");
        IsIsEnabled = false;
        try
        {
            await ippTask;
        }
        catch
        {
            /* already logged inside */
        }

        _logger.LogInformation("Stopped.");
    }

    // =========================================================================
    //  USB printer-class control requests (ep0)
    // =========================================================================

    private async Task HandleSetupAsync(FileStream ep0, byte[] ev, CancellationToken ct)
    {
        // ev[0..7] = usb_ctrlrequest: bmRequestType, bRequest, wValue(le16),
        //            wIndex(le16), wLength(le16)
        var bRequest = ev[1];
        var wLength = BinaryPrimitives.ReadUInt16LittleEndian(ev.AsSpan(6));

        switch (bRequest)
        {
            case ReqGetDeviceId:
            {
                byte[] id = Encoding.ASCII.GetBytes(DeviceId);
                ushort total = (ushort)(id.Length + 2);
                int send = Math.Min(total, wLength);
                byte[] resp = new byte[send];
                resp[0] = (byte)(total >> 8);
                resp[1] = (byte)(total & 0xFF);
                int copy = Math.Min(id.Length, send - 2);
                if (copy > 0) Buffer.BlockCopy(id, 0, resp, 2, copy);
                await ep0.WriteAsync(resp, ct);
                await ep0.FlushAsync(ct);

                _logger.LogInformation("GET_DEVICE_ID → {send} bytes", send);
                break;
            }
            case ReqGetPortStatus:
            {
                // bit5=NOT_ERROR, bit4=SELECT, bit3=PAPER_EMPTY(0=has paper)
                await ep0.WriteAsync(new byte[] { 0x30 }, ct);
                await ep0.FlushAsync(ct);

                _logger.LogInformation("GET_PORT_STATUS → 0x30");
                break;
            }
            case ReqSoftReset:
                // Accept; nothing to reset
                await ep0.WriteAsync(Array.Empty<byte>(), ct);
                await ep0.FlushAsync(ct);

                _logger.LogInformation("SOFT_RESET → ack");
                break;
            default:
                // Stall unknown requests
                await ep0.WriteAsync(Array.Empty<byte>(), ct);
                await ep0.FlushAsync(ct);

                _logger.LogInformation("Unknown bRequest 0x{bRequest:X2} → stall", bRequest);
                break;
        }
    }

    /// <summary>
    /// Bulk I/O loop: open bulk endpoints when enabled, then handle requests until disabled or cancellation requested.
    /// </summary>
    private async Task BulkLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            // Poll until the gadget is enumerated (host enabled the interface)
            while (!IsIsEnabled && !ct.IsCancellationRequested)
            {
                await Task.Delay(100, ct).ConfigureAwait(false);
            }

            if (ct.IsCancellationRequested)
            {
                break;
            }

            _logger.LogInformation("Opening bulk endpoints …");

            try
            {
                await using var epOut = new FileStream($"{FfsMount}/ep1", FileMode.Open, FileAccess.Read,
                    FileShare.None, bufferSize: 1);
                await using var epIn = new FileStream($"{FfsMount}/ep2", FileMode.Open, FileAccess.Write,
                    FileShare.None, bufferSize: 1, FileOptions.WriteThrough);

                _logger.LogInformation("Bulk endpoints open — ready for IPP jobs.");

                while (IsIsEnabled && !ct.IsCancellationRequested)
                {
                    await HandleOneRequestAsync(epOut, epIn, ct).ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception ex) when (IsIsEnabled)
            {
                _logger.LogError("Bulk I/O: {message}", ex.Message);
                await Task.Delay(500, ct).ConfigureAwait(false);
            }
            finally
            {
                _logger.LogInformation("Bulk endpoints closed.");
            }
        }
    }

    /// <summary>
    /// Handle one IPP-over-USB request: read HTTP headers and body from epOut, parse IPP request, build IPP response, write HTTP+IPP response to epIn.
    /// </summary>
    /// <exception cref="EndOfStreamException" />
    /// <exception cref="InvalidDataException" />
    private async Task HandleOneRequestAsync(FileStream epOut, FileStream epIn, CancellationToken ct)
    {
        var (headerText, bodyLeader, contentLength) = await ReadHttpHeadersAsync(epOut, ct).ConfigureAwait(false);

        var requestLine = headerText.Split('\n')[0].Trim();

        _logger.LogInformation("Received request: {requestLine} with Content-Length: {contentLength} bytes",
            requestLine, contentLength);

        byte[] body;
        if (contentLength <= 0)
        {
            body = bodyLeader;
        }
        else
        {
            body = new byte[contentLength];
            var have = Math.Min(bodyLeader.Length, (int)contentLength);
            Buffer.BlockCopy(bodyLeader, 0, body, 0, have);
            var remaining = (int)contentLength - have;
            while (remaining > 0)
            {
                var got = await epOut.ReadAsync(body.AsMemory(have, remaining), ct).ConfigureAwait(false);
                if (got <= 0) throw new EndOfStreamException("Short IPP body");
                have += got;
                remaining -= got;
            }
        }

        if (body.Length < 8)
            throw new InvalidDataException($"IPP body too short ({body.Length} bytes)");

        // ── 3. Parse IPP header ───────────────────────────────────────────────
        var ippOp = BinaryPrimitives.ReadUInt16BigEndian(body.AsSpan(2));
        var reqId = BinaryPrimitives.ReadUInt32BigEndian(body.AsSpan(4));
        _logger.LogInformation("IPP op=0x{ippOp:X4} req_id={reqId} body={bodyLength}B", ippOp, reqId, body.Length);

        // ── 4. Build IPP response ─────────────────────────────────────────────
        var ippResp = ippOp switch
        {
            0x000b => BuildGetPrinterAttrsResponse(reqId),
            0x0002 => BuildPrintJobResponse(reqId, Interlocked.Increment(ref _nextJobId)),
            0x0003 => BuildSimpleOkResponse(reqId), // Validate-Job
            0x000a => BuildGetJobsResponse(reqId), // Get-Jobs
            _ => BuildSimpleOkResponse(reqId),
        };

        // ── 5. Send HTTP 200 + IPP body ───────────────────────────────────────
        var httpHeader =
            $"HTTP/1.1 200 OK\r\n" +
            $"Content-Type: application/ipp\r\n" +
            $"Content-Length: {ippResp.Length}\r\n" +
            $"Connection: keep-alive\r\n\r\n";

        var httpBytes = Encoding.ASCII.GetBytes(httpHeader);
        await epIn.WriteAsync(httpBytes, ct).ConfigureAwait(false);
        await epIn.WriteAsync(ippResp, ct).ConfigureAwait(false);
        await epIn.FlushAsync(ct).ConfigureAwait(false);
    }

    /// <summary>
    /// Reads HTTP headers and the initial part of the body from a stream asynchronously.
    /// </summary>
    /// <exception cref="EndOfStreamException">Thrown if the end of the stream is reached before the HTTP headers are fully read.</exception>
    private async Task<(string headers, byte[] bodyLeader, long contentLength)> ReadHttpHeadersAsync(
        FileStream ep, CancellationToken ct)
    {
        var accumulator = new List<byte>(4096);
        var tmp = new byte[4096];

        while (!ct.IsCancellationRequested)
        {
            var totalNumberOfBytes = await ep.ReadAsync(tmp, ct).ConfigureAwait(false);
            if (totalNumberOfBytes <= 0) throw new EndOfStreamException("ep1 closed");

            for (var i = 0; i < totalNumberOfBytes; i++)
            {
                accumulator.Add(tmp[i]);
            }

            var sep = FindHeaderEnd(accumulator);
            if (sep < 0)
            {
                continue;
            }

            var headers = Encoding.ASCII.GetString(accumulator.ToArray(), 0, sep);

            var bodyLeader = accumulator.Count > sep + 4
                ? accumulator.GetRange(sep + 4, accumulator.Count - sep - 4).ToArray()
                : Array.Empty<byte>();

            // Parse Content-Length
            var cl = headers.Split('\n', StringSplitOptions.RemoveEmptyEntries)
                .Select(line => line.TrimStart())
                .Where(line => line.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase))
                .Select(line => long.TryParse(line.AsSpan("Content-Length:".Length).Trim(), out var cl) ? cl : 0)
                .FirstOrDefault(0);

            _logger.LogDebug(
                "HTTP headers read ({headers.Length} chars), body leader {bodyLeader.Length} bytes, Content-Length={cl}",
                headers, bodyLeader, cl);

            return (headers, bodyLeader, cl);
        }

        return (string.Empty, Array.Empty<byte>(), 0);
    }

    private static int FindHeaderEnd(List<byte> data)
    {
        for (var i = 0; i < data.Count - 3; i++)
        {
            if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n')
            {
                return i;
            }
        }

        return -1;
    }

    /// <summary>
    /// Build IPP USB gadget descriptors for both FS and HS modes, and package them in a FunctionFS header.
    /// </summary>
    private static byte[] BuildDescriptors()
    {
        // Interface descriptor (9 bytes)
        byte[] usbInterface =
        [
            9, UsbDtInterface,
            0, // bInterfaceNumber  (assigned by kernel)
            0, // bAlternateSetting
            2, // bNumEndpoints
            UsbClsPrinter, UsbScPrinter, UsbProtoIpp,
            0 // iInterface
        ];

        var fsEpOut = MakeEp(0x01, 64); // EP1 OUT, FS 64B
        var fsEpIn = MakeEp(UsbDirIn | 0x02, 64); // EP2 IN,  FS 64B
        var hsEpOut = MakeEp(0x01, 512); // EP1 OUT, HS 512B
        var hsEpIn = MakeEp(UsbDirIn | 0x02, 512); // EP2 IN,  HS 512B

        var fs = Concat(usbInterface, fsEpOut, fsEpIn);
        var hs = Concat(usbInterface, hsEpOut, hsEpIn);

        var totalLen = 20u + (uint)(fs.Length + hs.Length);

        using var ms = new MemoryStream();
        using var bw = new BinaryWriter(ms); // BinaryWriter = little-endian (correct for USB)
        bw.Write(FfsMagicDesc); // magic
        bw.Write(totalLen); // length
        bw.Write(FfsHasFs | FfsHasHs); // flags
        bw.Write(3u); // fs_count
        bw.Write(3u); // hs_count
        bw.Write(fs);
        bw.Write(hs);

        return ms.ToArray();
    }

    private static byte[] MakeEp(byte addr, ushort maxPkt) =>
    [
        7, UsbDtEndpoint, addr, UsbXFerBulk,
        (byte)(maxPkt & 0xFF), (byte)(maxPkt >> 8),
        0 // bInterval
    ];

    /// <summary>
    /// Builds a binary header for a string table with no entries and returns its byte representation.
    /// </summary>
    private static byte[] BuildStrings()
    {
        using var ms = new MemoryStream();
        using var bw = new BinaryWriter(ms);
        bw.Write(FfsMagicStr); // magic
        bw.Write(16u); // length (header only, no strings)
        bw.Write(0u); // str_count
        bw.Write(0u); // lang_count

        return ms.ToArray();
    }

    private static byte[] Concat(params byte[][] arrays)
    {
        var total = 0;
        foreach (var a in arrays) total += a.Length;
        var result = new byte[total];
        var off = 0;
        foreach (var a in arrays)
        {
            Buffer.BlockCopy(a, 0, result, off, a.Length);
            off += a.Length;
        }

        return result;
    }

    // =========================================================================
    //  IPP response builders
    // =========================================================================

    private static byte[] BuildGetPrinterAttrsResponse(uint reqId)
    {
        var b = new IppBuilder();
        b.RespHeader(reqId);
        b.U8(TagPrintGrp);
        b.EnumAttr("printer-state", 3); // idle
        b.KeyAttr("printer-state-reasons", "none");
        b.BoolAttr("printer-is-accepting-jobs", true);
        b.TextAttr("printer-info", "Raspberry Pi IPP USB Printer");
        b.TextAttr("printer-make-and-model", "RaspberryPi IPP USB Printer");
        b.UriAttr("printer-uri-supported", "ipp://localhost/ipp/print");
        b.KeyAttr("uri-authentication-supported", "none");
        b.KeyAttr("uri-security-supported", "none");
        b.IntAttr("printer-up-time", 1);
        b.MimeAttr("document-format-supported", "application/pdf");
        b.MimeAttr("document-format-default", "application/pdf");
        // Supported operations: Print-Job, Validate-Job, Cancel-Job,
        //   Get-Job-Attributes, Get-Jobs, Get-Printer-Attributes
        int[] ops = [0x0002, 0x0003, 0x0008, 0x0009, 0x000a, 0x000b];
        b.EnumAttr("operations-supported", ops[0]);
        for (int i = 1; i < ops.Length; i++) b.EnumAdditional(ops[i]);
        b.BoolAttr("multiple-document-jobs-supported", false);
        b.U8(TagEnd);
        return b.ToArray();
    }

    private static byte[] BuildPrintJobResponse(uint reqId, int jobId)
    {
        var b = new IppBuilder();
        b.RespHeader(reqId);
        b.U8(TagJobGrp);
        b.IntAttr("job-id", jobId);
        b.EnumAttr("job-state", 9); // completed
        b.KeyAttr("job-state-reasons", "job-completed-successfully");
        b.U8(TagEnd);
        return b.ToArray();
    }

    private static byte[] BuildGetJobsResponse(uint reqId)
    {
        // Return empty job list — we don't persist jobs
        var b = new IppBuilder();
        b.RespHeader(reqId);
        b.U8(TagEnd);
        return b.ToArray();
    }

    private static byte[] BuildSimpleOkResponse(uint reqId)
    {
        var b = new IppBuilder();
        b.RespHeader(reqId);
        b.U8(TagEnd);
        return b.ToArray();
    }

    // =========================================================================
    //  IPP binary builder (big-endian)
    // =========================================================================

    private sealed class IppBuilder
    {
        private readonly MemoryStream _ms = new();

        public void U8(byte v) => _ms.WriteByte(v);

        public void Be16(ushort v)
        {
            _ms.WriteByte((byte)(v >> 8));
            _ms.WriteByte((byte)(v & 0xFF));
        }

        public void Be32(uint v)
        {
            _ms.WriteByte((byte)(v >> 24));
            _ms.WriteByte((byte)((v >> 16) & 0xFF));
            _ms.WriteByte((byte)((v >> 8) & 0xFF));
            _ms.WriteByte((byte)(v & 0xFF));
        }

        private void Attr(byte tag, string? name, byte[] value)
        {
            U8(tag);
            byte[] nameBytes = name is not null
                ? Encoding.ASCII.GetBytes(name)
                : Array.Empty<byte>();
            Be16((ushort)nameBytes.Length);
            _ms.Write(nameBytes);
            Be16((ushort)value.Length);
            _ms.Write(value);
        }

        private static byte[] I32Be(int v)
        {
            var b = new byte[4];
            BinaryPrimitives.WriteInt32BigEndian(b, v);
            return b;
        }

        public void IntAttr(string name, int v) => Attr(TagInteger, name, I32Be(v));
        public void EnumAttr(string name, int v) => Attr(TagEnum, name, I32Be(v));
        public void EnumAdditional(int v) => Attr(TagEnum, null, I32Be(v));
        public void BoolAttr(string name, bool v) => Attr(TagBoolean, name, [v ? (byte)1 : (byte)0]);
        public void TextAttr(string name, string v) => Attr(TagText, name, Encoding.ASCII.GetBytes(v));
        public void KeyAttr(string name, string v) => Attr(TagKeyword, name, Encoding.ASCII.GetBytes(v));
        public void UriAttr(string name, string v) => Attr(TagUri, name, Encoding.ASCII.GetBytes(v));
        public void MimeAttr(string name, string v) => Attr(TagMime, name, Encoding.ASCII.GetBytes(v));

        public void RespHeader(uint requestId)
        {
            U8(0x02);
            U8(0x00); // IPP version 2.0
            Be16(0x0000); // status-code: successful-ok
            Be32(requestId);
            U8(TagOperGrp);
            Attr(TagCharset, "attributes-charset", Encoding.ASCII.GetBytes("utf-8"));
            Attr(TagLang, "attributes-natural-language", Encoding.ASCII.GetBytes("en"));
        }

        public byte[] ToArray() => _ms.ToArray();
    }
}