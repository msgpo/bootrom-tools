/*
 * Copyright (c) 2015 Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 *
 * @brief: This file contains common parsing support code
 *
 */
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include "util.h"
#include "ffff.h"
#include "parse_support.h"

/* Maximum length of a usage line to be displayed */
#define USAGE_LINE_LENGTH 80


typedef struct {
    const char *   string;
    const uint32_t value;
} parse_entry;


static parse_entry element_types[] = {
    {"s3fw", FFFF_ELEMENT_STAGE_2_FW},
    {"s2fw", FFFF_ELEMENT_STAGE_3_FW},
    {"icert", FFFF_ELEMENT_IMS_CERT},
    {"ccert", FFFF_ELEMENT_CMS_CERT},
    {"data", FFFF_ELEMENT_DATA},
    {"end", FFFF_ELEMENT_END},
};


/**
 * @brief Create an option table from an optionx table
 *
 * @param opt Pointer to the dst option entry to initialize
 * @param name Pointer to the name to use for the option
 * @param optx Pointer to the src optionx entry
 *
 * @returns Nothing
 */
void parse_args_init_opt_entry(struct option * opt, const char * name, struct optionx * optx) {
    opt->val = optx->short_name;
    opt->name = name;
    opt->flag = NULL;

    if (optx->flags & (STORE_FALSE | STORE_TRUE)) {
        opt->has_arg = no_argument;
    } else {
        opt->has_arg = required_argument;
    }
}


/**
 * @brief Create an option table from an optionx table
 *
 * @param argp Pointer to the parsing context
 *
 * @returns true on success, false on failure
 */
bool parse_args_init(struct argparse * argp) {
    struct option * opt;
    struct optionx * optx;

    if (!argp || !argp->optx || !argp->opt) {
        fprintf(stderr, "ERROR (getoptionx_init): Invalid args\n");
        return false;
    }

    opt = argp->opt;
    optx = argp->optx;

    /* Process the primary names */
    for (optx = argp->optx; optx->name != NULL; optx++, opt++) {
        if (optx->name) {
            /* Normal case */
            parse_args_init_opt_entry(opt, optx->name, optx);

            if (optx->flags & (STORE_FALSE | STORE_TRUE)) {
                optx->flags |= DEFAULT_VAL;
                optx->default_val = optx->flags & (STORE_FALSE)? true : false;
                if (!optx->callback) {
                    optx->callback = &store_flag;
                }
            }

            /* Initialize variable portions of the optx entry */
            optx->count = 0;
        }
    }

    /**
     * Process the secondary names (these are comma-separated in the name
     * field). What we will do is add additional "opt" entries, each pointing
     * into the name field, and we will change all of the commas in the name
     * field into null characters
     */
    if (argp->num_secondary_entries > 0) {
        for (optx = argp->optx; optx->name != NULL; optx++) {
            if (optx->name) {
                char * comma = optx->name;
                while (comma != NULL) {
                    comma = strchr(comma, ',');
                    if (comma != NULL) {
                        /*
                         * Convert the comma to a null and add an opt with
                         * the remaining string as the name. (Doing so will
                         * trim the previous entries pointing at the original
                         * comma-separated name.
                         */
                        *comma++ = '\0';
                        parse_args_init_opt_entry(opt, comma, optx);
                        opt++;
                     }
                }
            }
        }
    }

    /* Add the End-Of-Table marker to the opt array */
    opt->name = NULL;
    opt->has_arg = 0;
    opt->flag = NULL;
    opt->val = 0;

    return true;
}


/**
 * @brief Create an argparse structure, initialized from an optionx table
 *
 * @param optx Pointer to the src optionx table
 * @param prog The name of the program (typically argv.[0])
 * @param description (optional) Text to display before argument help
 * @param epilog (optional) Text to display after argument help
 * @param positional_arg_description (optional) Text to display at the end
 *        of the usage string for positional args
 * @param preprocess (optional) Pointer to a function which performs some
 *        check of the current "option" character before parse_args processes
 *        it further. (Typically used to close a multi-arg window.)
 *
 * @returns A pointer to the allocated struct on success, NULL on failure
 */
struct argparse * new_argparse(struct optionx *optx,
                               const char * prog,
                               const char * description,
                               const char * epilog,
                               const char * positional_arg_description,
                               PreprocessCallback preprocess) {
    struct optionx *scan = optx;
    int num_entries = 0;
    int num_secondary_entries = 0;
    struct argparse * argp = NULL;

    if (optx && prog) {
        /**
         *  Count the number of entries in optx, including the terminating entry
         */
        while (true) {
            num_entries++;
            if (scan->name == NULL) {
                break;
            } else {
                /**
                 * Count the number of secondary name strings in the
                 * comma-separated list
                 */
                char * comma = scan->name;
                while (comma != NULL) {
                    comma = strchr(comma, ',');
                    if (comma != NULL) {
                        num_secondary_entries++;
                        comma++;
                    }
                }
            }
            scan++;
        }

        /* Allocate the argparse and create its opt table */
        argp = malloc(sizeof(*argp));
        if (!argp) {
            fprintf(stderr, "ERROR(new_argparse): Can't allocate argparse\n");
        } else {
            argp->prog = prog;
            argp->description = description;
            argp->epilog = epilog;
            argp->positional_arg_description = positional_arg_description;
            argp->num_entries = num_entries;
            argp->num_secondary_entries = num_secondary_entries;
            argp->preprocess = preprocess;
            argp->optx = optx;
            argp->opt = calloc(num_entries + num_secondary_entries,
                               sizeof(struct option));
            if (!argp->opt) {
                fprintf(stderr,
                        "ERROR(new_argparse): Can't allocate option table\n");
                free(argp);
                argp = NULL;
            } else {
                parse_args_init(argp);
            }
        }
    }

    return argp;
}


/**
 * @brief Deallocate an argparse structure
 *
 * @param argp Pointer to the parsing context
 *
 * @returns NULL as a convenience to allow the caller to assign it to the
 *          supplied pointer
 */
struct argparse * free_argparse(struct argparse *argp) {
    if (argp) {
        free(argp->opt);
        free(argp);
    }
    return NULL;
}


/**
 * @brief Parse all of the args
 *
 * @param argc Count of command line args
 * @param argv The command line argument vector
 * @param optstring A string containing the legitimate option characters
 * @param parse_table The parsing table
 *
 * @returns True if there were no errors, false if there were
 */
bool parse_args(int argc, char * const argv[], const char *optstring,
                struct argparse *parse_table) {
    int option;
    int option_index;
    int total_entries;
    struct optionx *optx = NULL;
    bool success = true;

    total_entries = parse_table->num_entries +
            parse_table->num_secondary_entries;

    /* Parsing loop */
    while (true) {
        option = getopt_long_only (argc,
                          argv,
                          optstring,
                          parse_table->opt,
                          &option_index);
        /* End of options? */
        if (option == -1) {
            break;
        }

        /* Unrecognized option? */
        if (option == '?') {
            success = false;
            continue;
        }

        /**
         * Perform any global preprocesing before calling the appropriate callback.
         */
        if (parse_table->preprocess) {
            parse_table->preprocess(option);
        }


       /* Map secondary names into primary names */
       if ((option != 0) &&
           (option_index >= parse_table->num_entries) &&
           (option_index < total_entries)) {
           /**
            * The index belongs to one of the secondary names, scan through
            * the primary names for a matching option character
            */
           for (option_index = 0; option_index < parse_table->num_entries; option_index++) {
               if (option == parse_table->optx[option_index].short_name) {
                   break;
               }
           }
       }


       /* Process the option */
       if ((option != 0) &&
           (option_index >= 0) &&
           (option_index < parse_table->num_entries)) {
           struct optionx *optx = &parse_table->optx[option_index];

           /**
            *  Workaround for getopt_long's handling of args w/o values
            *  (e.g., flags).
            *  When getop_long encounters a short-named arg with,
            *  "opt->has_arg = no_argument", it returns an option_index of
            *  zero. The workaround is to check the option char against the
            *  short name, and if they don't match, search for it ourselves.
            */
           if (/*(option_index == 0) &&*/ (option != optx->short_name)) {
               for (optx = parse_table->optx; optx->name != NULL; optx++) {
                   if (optx->short_name == option) {
                       break;
                   }
               }
           }

           if (optx->callback) {
               optx->count++;
               if (!optx->callback(option, optarg, optx)) {
                   success = false;
               }
           }
        }
    }

    /* Post-parsing, apply defaults or squawk about missing params */
    for (optx = parse_table->optx; optx->name != NULL; optx++) {
        if (optx->count == 0) {
            /* Missing arg */
            if (optx->flags & REQUIRED) {
                fprintf (stderr, "ERROR: --%s is required\n", optx->name);
                success = false;
            } else if ((optx->flags & DEFAULT_VAL) && optx->var_ptr) {
                *(uint32_t*)optx->var_ptr = optx->default_val;
            }
        }
    }
    return success;
}


/**
 * @brief Generic parsing callback to store a hex number.
 *
 * @param option The option character (may be used to disambiguate a
 *        common function)
 * @param optarg The string from the argument parser
 * @param optx A pointer to the option descriptor
 *
 * @returns Returns true on success, false on failure
 */
bool store_hex(const int option, const char * optarg, struct optionx * optx) {
    if (optx->var_ptr) {
        return get_num(optarg, optx->name, (uint32_t *)optx->var_ptr);
    } else {
        fprintf(stderr, "ERROR: No var to store --%s", optx->name);
        return false;
    }
}


/**
 * @brief Generic parsing callback to store a string.
 *
 * @param option The option character (may be used to disambiguate a
 *        common function)
 * @param optarg The string from the argument parser
 * @param optx A pointer to the option descriptor
 *
 * @returns Returns true on success, false on failure
 */
bool store_str(const int option, const char * optarg, struct optionx * optx) {
    if (optx->var_ptr) {
        *(const char**)optx->var_ptr = optarg;
        return true;
    } else {
        fprintf(stderr, "ERROR: No var to store --%s", optx->name);
        return false;
    }
}


/**
 * @brief Generic parsing callback to store a flag.
 *
 * @param option The option character (may be used to disambiguate a
 *        common function)
 * @param optarg The string from the argument parser
 * @param optx A pointer to the option descriptor
 *
 * @returns Returns true on success, false on failure
 */
bool store_flag(const int option, const char * optarg, struct optionx * optx) {
    if (optx->var_ptr) {
        *(int*)optx->var_ptr = (optx->flags & STORE_TRUE)? true : false;
        return true;
    } else {
        fprintf(stderr, "ERROR: No var to store --%s", optx->name);
        return false;
    }
}


/**
 * @brief Parse a hex number.
 *
 * @param optarg The (hopefully) numeric string to parse (argv[i])
 * @param optname The name of the argument, used for error messages.
 * @param num Points to the variable in which to store the parsed number.
 *
 * @returns Returns true on success, false on failure
 */
bool get_num(const char * optarg, const char * optname, uint32_t * num) {
    char *tail = NULL;
    bool success = true;

    *num = strtoul(optarg, &tail, 0);
    if ((errno != errno) || ((tail != NULL) && (*tail != '\0'))) {
        fprintf (stderr, "Error: invalid %s '%s'\n", optname, optarg);
        success = false;
    }

    return success;
}


/**
 * @brief Parse an FFFF ElementType/TFTF PackageType.
 *
 * @param optarg The string to parse (argv[i])
 * @param type Points to the variable in which to store the parsed type.
 *
 * @returns Returns true on success, false on failure
 */
bool get_type(const char * optarg, uint32_t * type) {
    int i;

    for (i = 0; i < _countof(element_types); i++) {
        if (strcmp(optarg, element_types[i].string) == 0) {
            *type = element_types[i].value;
            return true;
        }
    }
    return false;
}


/**
 * @brief Print a usage message from the argparse info
 *
 * Automatically generates the usage message in a style inspired by Python's
 * "argparse"
 *
 * @param argp Pointer to the parsing context
 *
 * @returns Nothing
 */
void usage(struct argparse *argp) {
    if (argp) {
        char item_buf[256];
        int line_length;
        int item_length;
        struct optionx * optx;
        size_t longest_required_arg = 0;
        size_t longest_optional_arg = 0;
        bool issued_header;

        /* Print the usage string */
        line_length = fprintf(stderr, "usage: %s ", argp->prog);
        for (optx = argp->optx; optx->name != NULL; optx++) {
            /* Determine the longest arg names */
            size_t len = strlen(optx->name);
            if (optx->flags & REQUIRED) {
                if (len > longest_required_arg) {
                    longest_required_arg = len;
                }
            } else {
                if (len > longest_optional_arg) {
                    longest_optional_arg = len;
                }
            }

            /* Format this argument */
            item_length = snprintf(item_buf, sizeof(item_buf),
                                   " [--%s%s%s]",
                                   optx->name,
                                   (optx->val_name)? " " : "",
                                   (optx->val_name)? optx->val_name : "");
            if ((line_length + item_length) >= USAGE_LINE_LENGTH) {
                fprintf(stderr, "\n");
                line_length = 0;
            }
            fprintf(stderr, "%s", item_buf);
            line_length += item_length;
        }
        /* Optionally add the nargs section */
        if (argp->positional_arg_description) {
            item_length = strlen(argp->positional_arg_description);
            if ((line_length + item_length) >= USAGE_LINE_LENGTH) {
                fprintf(stderr, "\n");
                line_length = 0;
            }
            fprintf(stderr, "%s", argp->positional_arg_description);
            line_length += item_length;
        }
        /* Close off the last line of usage */
        if (line_length != 0) {
            fprintf(stderr, "\n");
        }

        /* Optionally print the description */
        if (argp->description) {
            fprintf(stderr, "\n%s\n", argp->description);
        }

       /* Print the (required) argument help */
        for (optx = argp->optx, issued_header = false;
             optx->name != NULL;
             optx++) {
            if (optx->flags & REQUIRED) {
                if(!issued_header) {
                    fprintf (stderr, "\narguments:\n");
                    issued_header = true;
                }
                fprintf(stderr, "  %*s  %s\n",
                        (int)longest_required_arg, optx->name, optx->help);
            }
        }

        /* Print the (optional) argument help */
        for (optx = argp->optx, issued_header = false;
              optx->name != NULL;
              optx++) {
             if (!(optx->flags & REQUIRED)) {
                 if(!issued_header) {
                     fprintf (stderr, "\noptional arguments:\n");
                     issued_header = true;
                 }
                fprintf(stderr, "  %*s  %s\n",
                        (int)longest_optional_arg, optx->name, optx->help);
            }
        }

        /* Optionally print the epilog */
        if (argp->epilog) {
            fprintf(stderr, "\n%s\n", argp->epilog);
        }
    }
}

