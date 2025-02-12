import json
import os

# -- Project information -----------------------------------------------------

project = 'Koffi'
copyright = '2022, Niels Martignène'
author = 'Niels Martignène'

root = None
version = None
revision = None
stable = None

names = ['../../src/koffi/package.json', '../package.json']

for name in names:
    filename = os.path.dirname(__file__) + '/' + name

    if not os.path.exists(filename):
        continue;

    with open(filename) as f:
        config = json.load(f)

        root = os.path.dirname(name)
        version = config['version']
        revision = config['version']
        stable = config['stable']

    break

if root is None:
    raise FileNotFoundError('Cannot find Koffi package.json')

# -- General configuration ---------------------------------------------------

extensions = [
    'myst_parser',
    'sphinx.ext.autosectionlabel'
]

# Add any paths that contain templates here, relative to this directory.
templates_path = ['templates']

exclude_patterns = []

# -- Options for HTML output -------------------------------------------------

html_title = project

html_theme = 'furo'

html_static_path = ['static']

html_theme_options = {
    'light_css_variables': {
        'color-brand-primary': '#FF6600',
        'color-brand-content': '#FF6600'
    },
    'dark_css_variables': {
        'color-brand-primary': '#FF6600',
        'color-brand-content': '#FF6600'
    }
}

html_link_suffix = ''

html_css_files = ['custom.css']

html_sidebars = {
    "**": [
        "sidebar/brand.html",
        "sidebar/search.html",
        "sidebar/scroll-start.html",
        "sidebar/navigation.html",
        "sidebar/ethical-ads.html",
        "badges.html",
        "sidebar/scroll-end.html",
        "sidebar/variant-selector.html"
    ]
}

html_context = {
    "root": root,
    "stable": stable
}

# -- MyST parser options -------------------------------------------------

myst_enable_extensions = [
    'linkify',
    'substitution'
]

myst_substitutions = html_context

myst_heading_anchors = 3

myst_linkify_fuzzy_links = False

myst_number_code_blocks = ['c', 'js', 'sh', 'batch']
