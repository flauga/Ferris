package com.aroleap.cardiodrum

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.aroleap.cardiodrum.ui.CardioViewModel
import com.aroleap.cardiodrum.ui.DashboardScreen
import com.aroleap.cardiodrum.ui.HistoryScreen
import com.aroleap.cardiodrum.ui.theme.Bg
import com.aroleap.cardiodrum.ui.theme.CardioTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            CardioTheme {
                AppRoot()
            }
        }
    }
}

/** Permissions a BLE scan+connect needs on this Android version. */
private fun requiredBlePermissions(): Array<String> =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    } else {
        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

@Composable
private fun AppRoot() {
    val navController = rememberNavController()
    // Activity-scoped ViewModel shared by both destinations.
    val vm: CardioViewModel = viewModel()

    val context = androidx.compose.ui.platform.LocalContext.current

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result.values.all { it }) vm.connect()
    }

    fun requestConnect() {
        val perms = requiredBlePermissions()
        val allGranted = perms.all {
            ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
        }
        if (allGranted) vm.connect() else permissionLauncher.launch(perms)
    }

    NavHost(
        navController = navController,
        startDestination = "dashboard",
        modifier = Modifier.fillMaxSize().background(Bg),
    ) {
        composable("dashboard") {
            DashboardScreen(
                vm = vm,
                onOpenHistory = { navController.navigate("history") },
                onRequestConnect = { requestConnect() },
            )
        }
        composable("history") {
            HistoryScreen(vm = vm, onBack = { navController.popBackStack() })
        }
    }
}
