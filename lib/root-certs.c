/* Bring the prefix's trusted root store up to date with the host's.
 *
 * Opening the Root store makes crypt32 import new host certificates. The
 * tuxoffice launcher runs this when the host's certificate bundle changes,
 * so the import happens here instead of inside Office, where Click-to-Run's
 * registry virtualization makes it fragile.
 */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>

int main(void)
{
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    PCCERT_CONTEXT cert = NULL;
    DWORD count = 0;

    if (!store) return 1;
    while ((cert = CertEnumCertificatesInStore(store, cert))) count++;
    CertCloseStore(store, 0);
    printf("%lu trusted root certificates\n", count);
    return 0;
}
