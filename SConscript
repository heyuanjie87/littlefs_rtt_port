# RT-Thread building script for component

from building import *

cwd = GetCurrentDir()

src = Glob('*.c')

CPPPATH = [cwd]

group = DefineGroup('fs/lfs', src , depend = ['RT_USING_DFS', 'PKG_USING_DFS_LFS'], CPPPATH = CPPPATH)

Return('group')
