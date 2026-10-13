#!/usr/bin/env python3
"""Compile actual extracted genpd functions against inert domain fixtures."""
from pathlib import Path
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'firmware/extracted/keep-boot-domains-source-20260915'
OUT = Path('/home/a6l/kernel/test-keep-boot-domain-functions-20260915')


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for i in range(opening, len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if not depth:
            return source[start:i + 1]
    raise AssertionError('Unterminated function')


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
struct device_node { int id; } node, other_node;
struct device { struct device_node *of_node; };
struct generic_pm_domain { struct device dev; int sync_state; bool stay_on; void *provider; } domain;
enum { GENPD_SYNC_STATE_OFF, GENPD_SYNC_STATE_ONECELL, GENPD_SYNC_STATE_SIMPLE };
static bool pd_ignore_unused, board_is_a6l;
static int gpd_list, gpd_list_lock, poweroffs, locks, logs;
#define list_for_each_entry(p, head, member) for ((p) = &domain; (p); (p) = NULL)
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define pr_info(...) do { logs++; } while (0)
static bool of_machine_is_compatible(const char *name) { return board_is_a6l && !strcmp(name,"hisense,hlte730t"); }
static void *of_fwnode_handle(struct device_node *np) { return np; }
static void mutex_lock(int *p) { locks++; }
static void mutex_unlock(int *p) { locks--; }
static void genpd_lock(struct generic_pm_domain *p) { locks++; }
static void genpd_unlock(struct generic_pm_domain *p) { locks--; }
static int genpd_power_off(struct generic_pm_domain *p, bool async, unsigned depth) { poweroffs++; return 0; }
'''

SUFFIX = r'''
int main(void) {
    unsigned cases = 0;
    for (int board=0; board<2; board++) for (int option=0; option<2; option++) {
        board_is_a6l=board; pd_ignore_unused=option;
        bool preserve=GUARDED && board && option;
        for (int route=0; route<6; route++) {
            poweroffs=locks=logs=0;
            domain=(struct generic_pm_domain){.dev={.of_node=&node},.stay_on=true,.provider=&node};
            switch(route) {
            case 0: of_genpd_sync_state(&node); break;
            case 1: domain.sync_state=GENPD_SYNC_STATE_ONECELL; genpd_provider_sync_state(&domain.dev); break;
            case 2: domain.sync_state=GENPD_SYNC_STATE_SIMPLE; genpd_provider_sync_state(&domain.dev); break;
            case 3: domain.sync_state=GENPD_SYNC_STATE_OFF; genpd_provider_sync_state(&domain.dev); break;
            case 4: of_genpd_sync_state(NULL); break;
            case 5: of_genpd_sync_state(&other_node); break;
            }
            int expected_off=!preserve && route<3;
            assert(poweroffs==expected_off);
            assert(domain.stay_on==!expected_off);
            assert(locks==0);
            if(preserve && route<3) assert(logs==1);
            cases++;
        }
    }
    printf("%u domain synchronization cases passed\n",cases);
    return 0;
}
'''


def main():
    OUT.mkdir(exist_ok=False)
    reports = {}
    for name, guarded in [('before', False), ('after', True)]:
        source = (SRC / ('core.' + name + '.c')).read_text()
        parts = []
        if guarded:
            parts.append(function(source, 'static bool a6l_keep_boot_domains(void)'))
        parts += [function(source, 'void of_genpd_sync_state(struct device_node *np)'),
                  function(source, 'static void genpd_provider_sync_state(struct device *dev)')]
        program = PREFIX + '\n'.join(parts) + '\n#define GUARDED ' + str(int(guarded)) + '\n' + SUFFIX
        path = OUT / (name + '.c')
        path.write_text(program)
        subprocess.run(['cc', '-std=gnu11', '-O2', str(path), '-o', str(OUT / name)], check=True)
        result = subprocess.run([str(OUT / name)], check=True, capture_output=True, text=True)
        reports[name] = result.stdout.strip()
    report = {'passed': True, 'scope': 'Actual extracted functions compiled with inert domain/lock stubs; both original and candidate tested. Hardware and locking concurrency are not emulated.',
              'checks': ['A6L plus pd_ignore_unused preserves stay_on and invokes no poweroff',
                         'Other board or absent option retains original poweroff behavior',
                         'Direct, onecell, simple, off, null and unmatched-provider routes',
                         'Original-code positive control exercises poweroff on A6L too'],
              'runs': reports}
    (SRC / 'function-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    for path in OUT.glob('*.c'):
        (SRC / ('test-' + path.name)).write_bytes(path.read_bytes())
    print(json.dumps(report))


if __name__ == '__main__':
    main()
