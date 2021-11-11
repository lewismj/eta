from distutils.core import setup

setup(name='eta',
      version="0.1",
      description="eta; minimalist Lisp/Scheme interpreter for Python.",
      author="Michael Lewis",
      author_email="lewismj@mac.com",
      url="https://github.com/lewismj/eta",
      package_dir =  {'eta': 'eta'},
      packages = ['eta'],
      install_requires=[
          'lark-parser=0.12.0',
          'prompt-toolkit==3.0.20',
          'pygments==2.10.0'
      ],
      license="BSD License",
      )
