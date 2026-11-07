The DER files are synthetic P-256 TLS test fixtures generated with OpenSSL.
`tls-key.der` matches the self-signed certificate; `tls-other-key.der` deliberately
does not. They contain no user or server credentials and are never packaged.

`identity-ca.der` is a test CA, and `identity-cert.der` / `identity-key.der` are
its leaf certificate and matching PKCS#8 key. The leaf has server-auth usage and
SANs for `lgnome-test.invalid` and `127.0.0.1`; PKI tests use a fixed time inside
its validity interval. These keys are public test data, never production secrets.
