/*
 * keygen is a small programs that generate a dnskey and private key
 * for a particular domain.
 *
 * (c) NLnet Labs, 2005 - 2008
 * See the file LICENSE for the license
 */

#include "config.h"

#include <ldns/ldns.h>

#include <errno.h>

void
usage(FILE *fp, char *prog) {
	fprintf(fp, "%s -a <algorithm> [-b bits] [-r /dev/random] [-v] domain\n",
		   prog);
	fprintf(fp, "  generate a new key pair for domain\n");
	fprintf(fp, "  -a <alg>\tuse the specified algorithm (-a list to");
	fprintf(fp, " show a list)\n");
	/*
	fprintf(fp, "  -D\t\tgenerate a DSA key\n");
	fprintf(fp, "  -R\t\tgenerate an RSA key\n");
	fprintf(fp, "  -H\t\tgenerate an HMAC-MD5 key (for TSIG)\n");
	*/
	fprintf(fp, "  -k\t\tset the flags to 257; key signing key\n");
	fprintf(fp, "  -b <bits>\tspecify the keylength\n");
	fprintf(fp, "  -r <random>\tspecify a random device (defaults to /dev/random)\n");
	fprintf(fp, "\t\tto seed the random generator with\n");
	fprintf(fp, "  -v\t\tshow the version and exit\n");
	fprintf(fp, "  The following files will be created:\n");
	fprintf(fp, "    K<name>+<alg>+<id>.key\tPublic key in RR format\n");
	fprintf(fp, "    K<name>+<alg>+<id>.private\tPrivate key in key format\n");
	fprintf(fp, "    K<name>+<alg>+<id>.ds\tDS in RR format (only for DNSSEC keys)\n");
	fprintf(fp, "  The base name (K<name>+<alg>+<id> will be printed to stdout\n");
}

void
show_algorithms(FILE *out)
{
	ldns_lookup_table *lt = ldns_signing_algorithms;
	fprintf(out, "Possible algorithms:\n");

	while (lt->name) {
		fprintf(out, "%s\n", lt->name);
		lt++;
	}
}

int
main(int argc, char *argv[])
{
	int c;
	char *prog;

	/* default key size */
	uint16_t def_bits = 1024;
	uint16_t bits = def_bits;
	bool ksk;

	FILE *file;
	FILE *random;
	char *filename;
	char *owner;

	ldns_signing_algorithm algorithm;
	ldns_rdf *domain;
	ldns_rr *pubkey;
	ldns_key *key;
	ldns_rr *ds;

	ldns_lookup_table *lt;

	prog = strdup(argv[0]);
	algorithm = 0;
	random = NULL;
	ksk = false; /* don't create a ksk per default */
	
	while ((c = getopt(argc, argv, "a:kb:r:v25")) != -1) {
		switch (c) {
		case 'a':
			if (algorithm != 0) {
				fprintf(stderr, "The -a argument can only be used once\n");
				exit(1);
			}
			if (strncmp(optarg, "list", 5) == 0) {
				show_algorithms(stdout);
				exit(EXIT_SUCCESS);
			}
			lt = ldns_signing_algorithms;
			while (lt->name) {
				if (strncasecmp(lt->name, optarg, strlen(lt->name)) == 0) {
					algorithm = lt->id;
				}
				lt++;
			}
			if (algorithm == 0) {
				fprintf(stderr, "Algorithm %s not found\n", optarg);
				show_algorithms(stderr);
				exit(EXIT_FAILURE);
			}
			break;
		case 'b':
			bits = (uint16_t) atoi(optarg);
			if (bits == 0) {
				fprintf(stderr, "%s: %s %d", prog, "Can not parse the -b argument, setting it to the default\n", (int) def_bits);
				bits = def_bits;
			}
			break;
		case 'k':
			ksk = true;
			break;
		case 'r':
			random = fopen(optarg, "r");
			if (!random) {
				fprintf(stderr, "Cannot open random file %s: %s\n", optarg, strerror(errno));
				exit(EXIT_FAILURE);
			}
			break;
		case 'v':
			printf("DNSSEC key generator version %s (ldns version %s)\n", LDNS_VERSION, ldns_version());
			exit(EXIT_SUCCESS);
			break;
		default:
			usage(stderr, prog);
			exit(EXIT_FAILURE);
		}
	}
	argc -= optind;
	argv += optind;

	if (algorithm == 0) {
		printf("Please use the -a argument to provide an algorithm\n");
		exit(1);
	}

	if (argc != 1) {
		usage(stderr, prog);
		exit(EXIT_FAILURE);
	} 
	free(prog);

	/* check whether key size is within RFC boundaries */
	switch (algorithm) {
	case LDNS_SIGN_RSAMD5:
	case LDNS_SIGN_RSASHA1:
		if (bits < 512 || bits > 4096) {
			fprintf(stderr, "For RSA, the key size must be between ");
			fprintf(stderr, " 512 and 4096 bytes. Aborting.\n");
			exit(1);
		}
		break;
	case LDNS_SIGN_DSA:
		if (bits < 512 || bits > 4096) {
			fprintf(stderr, "For DSA, the key size must be between ");
			fprintf(stderr, " 512 and 1024 bytes. Aborting.\n");
			exit(1);
		}
		break;
	case LDNS_SIGN_HMACMD5:
	default:
		break;
	}
	
	if (!random) {
		random = fopen("/dev/random", "r");
		if (!random) {
			fprintf(stderr, "Cannot open random file %s: %s\n", optarg, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	(void)ldns_init_random(random, (unsigned int) bits/8);
	fclose(random);

	/* create an rdf from the domain name */
	domain = ldns_dname_new_frm_str(argv[0]);

	/* generate a new key */
	key = ldns_key_new_frm_algorithm(algorithm, bits);
	
	/* set the owner name in the key - this is a /seperate/ step */
	ldns_key_set_pubkey_owner(key, domain);

	/* ksk flag */
	if (ksk) {
		ldns_key_set_flags(key, ldns_key_flags(key) + 1);
	}

	/* create the public from the ldns_key */
	pubkey = ldns_key2rr(key);
	if (!pubkey) {
		fprintf(stderr, "Could not extract the public key from the key structure...");
		ldns_key_deep_free(key);
		exit(EXIT_FAILURE);
	}
	owner = ldns_rdf2str(ldns_rr_owner(pubkey));
	
	/* calculate and set the keytag */
	ldns_key_set_keytag(key, ldns_calc_keytag(pubkey));

	/* build the DS record */
	ds = ldns_key_rr2ds(pubkey, LDNS_SHA1);

	/* print the public key RR to .key */
	filename = LDNS_XMALLOC(char, strlen(owner) + 17);
	snprintf(filename, strlen(owner) + 16, "K%s+%03u+%05u.key", owner, algorithm, (unsigned int) ldns_key_keytag(key));
	file = fopen(filename, "w");
	if (!file) {
		fprintf(stderr, "Unable to open %s: %s\n", filename, strerror(errno));
		ldns_key_deep_free(key);
		free(owner);
		ldns_rr_free(pubkey);
		ldns_rr_free(ds);
		LDNS_FREE(filename);
		exit(EXIT_FAILURE);
	} else {
		ldns_rr_print(file, pubkey);
		fclose(file);
		LDNS_FREE(filename);
	}
	
	/* print the priv key to stderr */
	filename = LDNS_XMALLOC(char, strlen(owner) + 21);
	snprintf(filename, strlen(owner) + 20, "K%s+%03u+%05u.private", owner, algorithm, (unsigned int) ldns_key_keytag(key));
	file = fopen(filename, "w");
	if (!file) {
		fprintf(stderr, "Unable to open %s: %s\n", filename, strerror(errno));
		ldns_key_deep_free(key);
		free(owner);
		ldns_rr_free(pubkey);
		ldns_rr_free(ds);
		LDNS_FREE(filename);
		exit(EXIT_FAILURE);
	} else {
		ldns_key_print(file, key);
		fclose(file);
		LDNS_FREE(filename);
	}
	
	/* print the DS to .ds */
	if (algorithm != LDNS_SIGN_HMACMD5) {
		filename = LDNS_XMALLOC(char, strlen(owner) + 16);
		snprintf(filename, strlen(owner) + 15, "K%s+%03u+%05u.ds", owner, algorithm, (unsigned int) ldns_key_keytag(key));
		file = fopen(filename, "w");
		if (!file) {
			fprintf(stderr, "Unable to open %s: %s\n", filename, strerror(errno));
			ldns_key_deep_free(key);
			free(owner);
			ldns_rr_free(pubkey);
			ldns_rr_free(ds);
			LDNS_FREE(filename);
			exit(EXIT_FAILURE);
		} else {
			ldns_rr_print(file, ds);
			fclose(file);
			LDNS_FREE(filename);
		}
	}	
#if 0
	/* TEMP: create PEM format too */
	filename = LDNS_XMALLOC(char, strlen(owner) + 17);
	snprintf(filename, strlen(owner) + 16, "K%s+%03u+%05u.pem", owner, algorithm, ldns_key_keytag(key));
	file = fopen(filename, "w");
	if (!file) {
		fprintf(stderr, "Unable to open %s: %s\n", filename, strerror(errno));
		exit(EXIT_FAILURE);
	} else {
				
		PEM_write_DSAPrivateKey(file, key->_key.dsa, NULL, NULL, 0, NULL, NULL);
		fclose(file);
		LDNS_FREE(filename);
	}
#endif

	fprintf(stdout, "K%s+%03u+%05u\n", owner, algorithm, (unsigned int) ldns_key_keytag(key));
	ldns_key_deep_free(key);
	free(owner);
	ldns_rr_free(pubkey);
	ldns_rr_free(ds);
	exit(EXIT_SUCCESS);
}
