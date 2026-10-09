NAME = 'v11'
EXE_SHA256 = 'c92298b62fc04fe66a0270fbaf082d7d7a5e37d0b5b7ed5adafccd43b26a67d7'
PATCHES = {
    0x6638: ('558bec', 'ca0200'),
}
EXE_SIZE = 259583
OVL_DESC = 150523
N_OVL = 16
ROOT_END = 0x251B
HOOKS = {
    ('root', 0x053b, 0x060a): 'rt_menu_save_enter',
    ('root', 0x053b, 0x0695): 'rt_menu_exit',
    ('root', 0x053b, 0x0544): 'rt_menu_load_enter',
    ('root', 0x053b, 0x0609): 'rt_menu_exit',
    ('o2', 0x29ef, 0x00a6): 'rt_cutscene_poll',
    ('root', 0x0289, 0x0120): 'rt_copyprot_ran',
    ('root', 0x1129, 0x1587): 'rt_dissolve_row',
}
FILES = {
    'prince.exe': (259583, 'c92298b62fc04fe66a0270fbaf082d7d7a5e37d0b5b7ed5adafccd43b26a67d7', ()),
    'prince.dat': (388168, '9b333abaeb9bd75b3d35ae8ead985143fd055fa64494c651bdd456f1d876dbb0', ()),
    'presets.def': (2049, 'dfb1c72afed284489276ca12647be605f2852de09dca2a7bad598bcdfd768ad2', ('snddrvrs/preset33.def',)),
    'msb_pro.drv': (2432, '2e55c6a346e63fffdd03fc1b70c17a40f5685f22ac721ecd6c2fa1f0fc5e099b', ('snddrvrs/msb_pro.drv', 'midi.drv')),
    'dsb_pro.drv': (2224, 'a4aaa8b0fbda27bebdb4ab9472212908aad1e41d7dcd111a5c79d3c4c6e12517', ('snddrvrs/dsb_pro.drv', 'digi.drv')),
    'sequence.dat': (11980, 'd46d334fec429b0722c462af76c0c2222e0e925d7348d0a68dd45239fa5e5598', ()),
    'kid.dat': (79395, '26ceade87556de83ca6a00e3cf49d0fa51a760104893b9b9be96cf5c486f63e7', ()),
    'guard.dat': (27299, 'bae163bc507ba09bfe5900429956116478dd4682f0c414d3d993c772105899d5', ()),
    'head.dat': (20084, 'cddbae5d1efdcbec95fb2089a5f55c2faf4a1b856cb9696cf5fe73214fb5229c', ()),
    'bird.dat': (32151, '5ff7cf078a93e9a9e2cd4548cfc6697f5262cb362b8a401eb3cb41ce0bfe7fe5', ()),
    'flame.dat': (3126, '54ac1764defa54c940e4f4fa80d14eae01c85ad576672306064d2c217782ef83', ()),
    'jinnee.dat': (3568, 'e35c61588dbe80bd5e461e8e4e2592d62038aab5824dfa56ab58e20e26c482df', ()),
    'skeleton.dat': (13744, 'd7c7ece66e1590d4d81e328ddcf9905892cd38f03be990cb7615cac7a9a56c42', ()),
    'final.dat': (401268, '24798bd215aac7854dcd3b2c66045b7bc00cd888f39ae185adee0fffe7cc171e', ()),
    'nis.dat': (1083866, '12efd97036f6ee77548207cd12010764d6af42166bddd67feb911e86b2a535ae', ()),
    'nis3vc.dat': (124155, '35a93fe9982f4bfaec3eb5886912365c8d211752b05c0d196a05dcefc75cfb98', ()),
    'nisdigi.dat': (1118400, '98ff1100f1e2afd13784b19ed92a6d7cdb8d83648e6c8a9cbbd82d9dc05e7628', ()),
    'nisibm.dat': (12168, '64720ab9280f492e76da1486c6ae7b21ca2dcfbef6597985578e102e735987d3', ()),
    'nismidi.dat': (174611, 'cc9f9d07194497f8c7b65df8fba0969f3d332332a39e7de0da775b3cbf65ae27', ()),
    'digisnd.dat': (669947, 'b3a47274a902e2b7a3c156c37701b3215869fbf3df42bf6012fb77573438a7c5', ()),
    'midisnd.dat': (414088, 'cc4caa3d6452a5826f4d5b003eb5748b9d23f1bacce6c79e3764af0935c69eab', ()),
    'ibmsnd.dat': (33142, 'ec6eba3b5a153e5f68b561af7eaa3ab90ff1dbdb0805cfa9e6a77678e62d1cca', ()),
    'rooftops.dat': (146028, 'bd71bee849e6402921d79952be0c6881600579e1cee3391d81de6062e6bcbde6', ()),
    'ruins.dat': (273461, '731ac4a8d41812dea9b12d8a8fcfb8223633741595c49b65d75f2f7120d95feb', ()),
    'temple.dat': (116801, '9e8e1bb576fcb50598042052080b91664440719661bfaeb335bf3b64f0563fe1', ()),
    'caverns.dat': (183189, 'eaa57b2300143ec8ceace831b9a174d5743d3f319b741e724696158929355b43', ()),
    'desert.dat': (104211, 'd3278fe0f08c07dc873db0d4217d58fb146233b2943d8dab60f82219ef0ac44c', ()),
    'trans.dat': (485491, '9359d0f808181399096c5a21b35a56b5bc9f9e27523d4f3394fc52964d065b40', ()),
}
OTHER_EXES = {
    'daa7c40e': 'the cracked v1.1',
    '17d0ddcb': 'v1.0', '8911c366': 'v1.0', '4080c635': 'v1.0',
    '2a4268b4': 'the initial release', '29582e3f': 'the initial release',
}
ENTRIES = 'extra_entries_v11.txt'
