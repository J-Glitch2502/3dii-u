"""Regression checks for omitted optimized shader packs and bounded pack selection."""
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import production_fallbacks as production


class ProductionFallbackTests(unittest.TestCase):
    def test_missing_new_compiler_is_actionable(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict(production.os.environ,{'THREEDII_FALLBACK_GLSLC':str(Path(directory)/'missing')}):
                with self.assertRaisesRegex(ValueError,'legacy shader compiler'):
                    production.compiler_path()

    def test_budget_keeps_complete_programs_and_counts_omissions(self):
        header=bytes(32); end=(b'end',b'')
        blocks=[(6,b'header',bytes(10)),(7,b'program',bytes(12)),
                (6,b'header',bytes(10)),(7,b'program',bytes(12)),(1,*end)]
        builder=type('Builder',(),{'gfd_blocks':lambda self,data:blocks})()
        with tempfile.TemporaryDirectory() as directory:
            output=Path(directory)/'subset.bin'
            with patch.object(production,'read_packet',return_value=([10,11],header)), \
                 patch('build_fallback_pack.builder_module',return_value=builder), \
                 patch.object(production,'write_packet') as write:
                kept,omitted=production.prefix_packet('input',output,120)
                self.assertEqual((kept,omitted),(1,1))
                self.assertEqual(write.call_args.args[1],[10])
                self.assertTrue(write.call_args.args[2].endswith(b'program'+bytes(12)+b'end'))
                self.assertEqual(production.prefix_packet('input',output,20),(0,2))
                self.assertEqual(production.prefix_packet('input',output,10000,1),(1,1))

    def test_regular_shader_build_requires_integration(self):
        # This boundary was absent in the faulty build: exercise build_shaders
        # with its compilation mocked, while requiring real integration dispatch.
        import sys,types
        sys.path.insert(0,str(production.ROOT/'tools/win'))
        import build3ds as builder
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'game/shader_logs').mkdir(parents=True)
            g=types.SimpleNamespace(dir=str(root/'game'),shaders=str(root/'shaders'),name='test',pipelines=None)
            (root/'work/shadergen').mkdir(parents=True)
            from contextlib import ExitStack
            with ExitStack() as stack:
                for name,value in [('GLSLC',__file__),('WORK',str(root/'work'))]:stack.enter_context(patch.object(builder,name,value))
                for name in ['prepare_logs','compile_shaders']:stack.enter_context(patch.object(builder,name))
                stack.enter_context(patch.object(builder,'build_shadergen',return_value='generator'))
                stack.enter_context(patch.object(builder,'run',return_value=types.SimpleNamespace(stdout='',returncode=0)))
                import shader_diagnostics,observed_fragments,shader_coverage
                for module,name in [(shader_diagnostics,'write_report'),(observed_fragments,'inject'),(shader_coverage,'check')]:stack.enter_context(patch.object(module,name))
                integrate=stack.enter_context(patch.object(production,'integrate'))
                builder.build_shaders(g,types.SimpleNamespace(jobs=2))
                integrate.assert_called_once_with(Path(g.shaders),Path('generator'),root/'work',2)


if __name__=='__main__':unittest.main()
