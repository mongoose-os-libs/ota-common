# OTA Core Library

This library contains the core OTA functionality.

It orchestrates the update process, reads data from source, parses the ZIP bundle and passes the parts on to one or more registered backends.

## Signed Updates

A flexible update signing mechanism is provided.

An update can carry 0 to 8 signatures, of which 0 to 8 may be required for update to proceed.

### Generating Keys

Currently the only supported signature format is ECDSA based on the NIST P-256 curve.

#### OpenSSL

Keys can be generated using the OpenSSL command line interface:

```
$ openssl ecparam -genkey -name prime256v1 -text -out k0.pem
```
`k0.pem` contains the private key and will be used for signing bundles.

Public part can be obtained as follows:
```
$ openssl ec -in k0.pem -pubout
read EC key
writing EC key
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEUHBb0BcfAs6zDm092wEsMnZsI/96
fEu+RO+4AdA9aOBzjfGMWLAPReP7gTl0nHVU+rjnYAfELZAjnarHt3pSYA==
-----END PUBLIC KEY-----
```

This part will be configured on the device in one of the key slots (begin and end markers and the line break need to be removed).

#### ssh-keygen

Keys can also be generated with `ssh-keygen`:

```
$ ssh-keygen -t ecdsa -m PEM
```

Feel free to set a passphrase to protect the key but you will need to enter it each time during signing.

Note: The auto-generated public key from `k0.pem.pub` cannot be used directly,
you still need to use `openssl ec -in k0.pem -pubout` to get the public key in a suitable representation.

### Adding Signatures to the Bundle

`mos` tool is used to add signatures to a firmware bundle.

Using the `create-fw-bundle` command, signatures can be added by specifying one or more `--sign-key` parameters:

```
$ mos create-fw-bundle -i build/fw.zip -o build/fw-signed.zip --sign-key=k0.pem
```

This adds two signatures, 0 and 1:

```
$ mos create-fw-bundle -i build/fw.zip -o build/fw-signed.zip --sign-key=k0.pem --sign-key=k1.pem
```


This adds two signatures, 0 and 2 (note the empty --sign-key in place of signature 1):

```
$ mos create-fw-bundle -i build/fw.zip -o build/fw-signed.zip --sign-key=k0.pem --sign-key= --sign-key=k2.pem
```

Note: Encrypted keys are supported. In this case you will be prompted for a password.

### Configuring the Device

There are 8 key slots on the device, `update.key0` to `update.key7`. These are string values, taking ECDSA P-256 public keys in Base64-encoded ASN.1 representation.
From the example above, the value to use would be `MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEUHBb0BcfAs6zDm092wEsMnZsI/96fEu+RO+4AdA9aOBzjfGMWLAPReP7gTl0nHVU+rjnYAfELZAjnarHt3pSYA==`.

Once a key is configured, the corresponding signature will be verified on update and result recorded in a bitmask, with each bit corresponding to a key: bit 0 to key 0, etc.

The simplest way to use this result is to configure `update.sig_req_mask`. The default is to ignore the verification result, but if set to a positive value,
the mask will be and-ed with the verification result. All bits of the mask must be set in the result, that is to say, all the signatures required by the mask must be valid.

Verification result is also made available to the user through the `MGOS_EVENT_OTA_BEGIN` event, in the `mi.sig_check_result` field.
Library user has full control on how to a lot of flexibility when it comes to using the verification results. For example, some keys may be used as special feature keys, etc.

Note: It is possible to set `update.sig_req_mask=0`, in which case no updates will be allowed. This can be used as a "tombstone" value under some circumstances.

### Key Rotation Example

Multiple key slots in combination with partial mask values can be used to set up a robust key rotation system.

Consider a device that has all 8 keys configured and `update.sig_req_mask=1`. This means that all updates must be signed by key 0 to be accepted.

Now let's say that key 0 has been compromised or needs to be rotated out for other reasons.
To facilitate this, an update is created with `update.sig_req_mask=2`. In order to be accepted by the current generation of devices it still needs to be signed by key 0,
but from now only updates signed by key 1 will be accepted. Updates can continue to be signed by both keys as long as backward compatibility is required.
