# ############################################################
# Importing - Same For All Render Layer Tests
# ############################################################

from render_layer_common import *
import unittest
import os
import sys

sys.path.append(os.path.dirname(__file__))


# ############################################################
# Testing
# ############################################################

class UnitTesting(MoveLayerCollectionTesting):
    def get_reference_scene_tree_map(self):
        # original tree, no changes
        reference_tree_map = [
                ['A', [
                    ['i', None],
                    ['ii', None],
                    ['iii', None],
                    ]],
                ['B', None],
                ['C', [
                    ['1', None],
                    ['2', None],
                    ['3', [
                        ['dog', None],
                        ['cat', None],
                        ]],
                    ]],
                ]
        return reference_tree_map

    def get_reference_layers_tree_map(self):
        # original tree, no changes
        reference_layers_map = [
                ['Layer 1', [
                    'Master Collection',
                    'C',
                    '3',
                    ]],
                ['Layer 2', [
                    'C',
                    '3',
                    'dog',
                    'cat',
                    ]],
                ]
        return reference_layers_map

    def test_layer_collection_into(self):
        """
        Test outliner operations
        """
        self.setup_tree()
        self.assertFalse(self.move_into("Layer 1.C.2", "Layer 2.3"))
        self.compare_tree_maps()


# ############################################################
# Main - Same For All Render Layer Tests
# ############################################################

if __name__ == '__main__':
    import sys

    extra_arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 2:] if "--" in sys.argv else [])

    UnitTesting._extra_arguments = extra_arguments
    unittest.main()