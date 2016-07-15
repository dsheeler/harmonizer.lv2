#!/usr/bin/env python
from waflib.extras import autowaf as autowaf
import re

# Variables for 'waf dist'
APPNAME = 'harmonizer.lv2'
VERSION = '0.0.1'

# Mandatory variables
top = '.'
out = 'build'

def options(opt):
    opt.load('compiler_c')
    opt.load('compiler_cxx')
    autowaf.set_options(opt)

def configure(conf):
    conf.load('compiler_c')
    conf.load('compiler_cxx')
    autowaf.configure(conf)
    autowaf.display_header('Harmonizer Configuration')
    conf.env.append_value('INCLUDES', ['/usr/local/include/stk'])
    conf.check_cxx(lib = 'stk')
    if not autowaf.is_child():
        autowaf.check_pkg(conf, 'lv2', atleast_version='1.2.1', uselib_store='LV2')
        autowaf.check_pkg(conf, 'aubio', uselib_store='AUBIO')
    # Set env['pluginlib_PATTERN']
    pat = conf.env['cshlib_PATTERN']
    if pat.startswith('lib'):
        pat = pat[3:]
    conf.env['pluginlib_PATTERN'] = pat
    conf.env['pluginlib_EXT'] = pat[pat.rfind('.'):]

    autowaf.display_msg(conf, 'LV2 bundle directory', conf.env.LV2DIR)
    print('')

def build(bld):
    bundle = APPNAME

    # Build manifest.ttl by substitution (for portable lib extension)
    bld(features     = 'subst',
        source       = 'manifest.ttl.in',
        target       = '%s/%s' % (bundle, 'manifest.ttl'),
        install_path = '${LV2DIR}/%s' % bundle,
        LIB_EXT      = bld.env['pluginlib_EXT'])

    # Copy other data files to build bundle (build/harmonizer.lv2)
    for i in ['harmonizer.ttl']:
        bld(features     = 'subst',
            is_copy      = True,
            source       = i + '.in',
            target       = '%s/%s' % (bundle, i),
            install_path = '${LV2DIR}/%s' % bundle,
            LIB_EXT      = bld.env['pluginlib_EXT'])

    # Create a build environment that builds module-style library names
    # e.g. triceratops.so instead of libtriceratops.so
    # Note for C++ you must set cxxshlib_PATTERN instead
    penv                   = bld.env.derive()
    penv['cshlib_PATTERN'] = bld.env['pluginlib_PATTERN']
    penv['cxxshlib_PATTERN'] = bld.env['pluginlib_PATTERN']

    # Use LV2 headers from parent directory if building as a sub-project
    includes = ['.']
    if autowaf.is_child:
        includes += ['../..']

    # Build plugin library
    obj = bld(features     = 'cxx cshlib',
              env          = penv,
              source       = 'harmonizer.cpp utilities.cpp',
              name         = 'harmonizer',
              target       = '%s/harmonizer' % bundle,
              install_path = '${LV2DIR}/%s' % bundle,
              cxxflags     = '-fPIC',
              use          = 'LV2 AUBIO',
              includes     = includes)
    #obj.env.cshlib_PATTERN = module_pat
    bld.install_files('${LV2DIR}/harmonizer.lv2/', bld.path.ant_glob('build/*'))
