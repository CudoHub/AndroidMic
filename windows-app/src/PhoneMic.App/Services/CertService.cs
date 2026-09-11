using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using PhoneMic.Core.Crypto;

namespace PhoneMic.App.Services;

/// <summary>
/// Самоподписанный TLS-сертификат сервера (RSA-3072, CN=PhoneMic-&lt;host&gt;, 10 лет).
/// Хранится в %LOCALAPPDATA%\PhoneMic\server.pfx; отпечаток SHA-256 — для пиннинга.
/// </summary>
public static class CertService
{
    private static readonly string Dir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PhoneMic");
    private static readonly string PfxPath = System.IO.Path.Combine(Dir, "server.pfx");

    private static X509Certificate2? _cert;

    public static X509Certificate2 GetOrCreate()
    {
        if (_cert != null) return _cert;
        var settings = SettingsService.Load();
        Directory.CreateDirectory(Dir);
        if (File.Exists(PfxPath))
        {
            try
            {
                _cert = new X509Certificate2(PfxPath, settings.PfxPassword,
                    X509KeyStorageFlags.UserKeySet | X509KeyStorageFlags.PersistKeySet);
                if (_cert.NotAfter > DateTime.Now.AddMonths(1)) return _cert;
                _cert.Dispose();
            }
            catch (Exception ex)
            {
                Core.Logging.AppLog.Warn("Cert", "pfx load failed, regenerate: " + ex.Message);
            }
        }
        var cn = $"PhoneMic-{Environment.MachineName}";
        using var rsa = RSA.Create(3072);
        var req = new CertificateRequest($"CN={cn}", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var cert = req.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(10));
        var pfx = cert.Export(X509ContentType.Pfx, settings.PfxPassword);
        File.WriteAllBytes(PfxPath, pfx);
        cert.Dispose();
        _cert = new X509Certificate2(PfxPath, settings.PfxPassword,
            X509KeyStorageFlags.UserKeySet | X509KeyStorageFlags.PersistKeySet);
        Core.Logging.AppLog.Info("Cert", $"created: CN={cn}, fp={TokenUtil.CertFingerprintHex(_cert)[..16]}…");
        return _cert;
    }

    public static string Fingerprint()
    {
        try { return TokenUtil.CertFingerprintHex(GetOrCreate()); }
        catch { return ""; }
    }
}
