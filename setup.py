from distutils.core import setup

setup(name='eta',
      version="0.1",
      description="eta; minimalist Lisp/Scheme interpreter for Python.",
      author="Michael Lewis",
      author_email="lewismj@mac.com",
      url="https://github.com/lewismj/eta",
      package_dir =  {'eta': 'eta'},
      packages = ['eta'],
      license="BSD License",
      )
