.. SPDX-License-Identifier: GPL-2.0

====================
mlx5 devlink support
====================

This document describes the devlink features implemented by the ``mlx5``
device driver.

Parameters
==========

.. list-table:: Generic parameters implemented

   * - Name
     - Mode
     - Validation
   * - ``enable_roce``
     - driverinit
     - Type: Boolean
   * - ``io_eq_size``
     - driverinit
     - The range is between 64 and 4096.
   * - ``event_eq_size``
     - driverinit
     - The range is between 64 and 4096.
   * - ``max_macs``
     - driverinit
     - The range is between 1 and 2^31. Only power of 2 values are supported.

The ``mlx5`` driver also implements the following driver-specific
parameters.

.. list-table:: Driver-specific parameters implemented
   :widths: 5 5 5 85

   * - Name
     - Type
     - Mode
     - Description
   * - ``flow_steering_mode``
     - string
     - runtime
     - Controls the flow steering mode of the driver

       * ``dmfs`` Device managed flow steering. In DMFS mode, the HW
         steering entities are created and managed through firmware.
       * ``smfs`` Software managed flow steering. In SMFS mode, the HW
         steering entities are created and manage through the driver without
         firmware intervention.

The ``mlx5`` driver supports reloading via ``DEVLINK_CMD_RELOAD``

Info versions
=============

The ``mlx5`` driver reports the following versions

.. list-table:: devlink info versions implemented
   :widths: 5 5 90

   * - Name
     - Type
     - Description
   * - ``fw.psid``
     - fixed
     - Used to represent the board id of the device.
   * - ``fw.version``
     - stored, running
     - Three digit major.minor.subminor firmware version number.
