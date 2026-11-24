package com.heartbeacon.app.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.heartbeacon.app.BuildConfig
import com.heartbeacon.app.data.HeartRepo
import com.heartbeacon.app.ui.heart.BeaconFigure
import com.heartbeacon.app.ui.heart.Stage
import kotlinx.coroutines.launch

class LoginVm(private val repo: HeartRepo) : ViewModel() {
    var user by mutableStateOf("")
    var pass by mutableStateOf("")
    var host by mutableStateOf(BuildConfig.DEFAULT_HOST)
    var advanced by mutableStateOf(false)
    var showPass by mutableStateOf(false)
    var busy by mutableStateOf(false)
        private set
    var error by mutableStateOf<String?>(null)
        private set

    val canSubmit get() = user.isNotBlank() && host.isNotBlank() && !busy

    fun submit() {
        if (!canSubmit) return
        busy = true
        error = null
        viewModelScope.launch {
            error = repo.login(user, pass, host)
            busy = false
        }
    }
}

@Composable
fun LoginScreen(vm: LoginVm) {
    StatusBarIcons(dark = false)
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
      Column(Modifier.imePadding().verticalScroll(rememberScrollState())) {
        // The beacon itself, unlit on its cloth, with the app's name set over the foot of the stage.
        Box(Modifier.fillMaxWidth().height(372.dp).clip(RoundedCornerShape(bottomStart = 28.dp, bottomEnd = 28.dp))) {
            BeaconFigure(play = null, modifier = Modifier.fillMaxSize(), fit = 0.285f, at = 0.44f)
            Text(
                "Heart Beacon",
                style = MaterialTheme.typography.displaySmall,
                color = Stage.text,
                modifier = Modifier.align(Alignment.BottomStart).padding(start = Space.xl, end = Space.xl, bottom = Space.xl),
            )
        }
        Column(
            Modifier.navigationBarsPadding().padding(start = Space.xl, end = Space.xl, top = Space.xl, bottom = Space.xl),
            verticalArrangement = Arrangement.spacedBy(Space.l),
        ) {
            Text(
                "Sign in with the account your admin set up for you.",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedTextField(
                value = vm.user,
                onValueChange = { vm.user = it },
                label = { Text("Username") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = vm.pass,
                onValueChange = { vm.pass = it },
                label = { Text("Password") },
                singleLine = true,
                visualTransformation = if (vm.showPass) VisualTransformation.None else PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { vm.submit() }),
                trailingIcon = {
                    TextButton(onClick = { vm.showPass = !vm.showPass }) { Text(if (vm.showPass) "Hide" else "Show") }
                },
                modifier = Modifier.fillMaxWidth(),
            )
            AnimatedVisibility(vm.advanced) {
                OutlinedTextField(
                    value = vm.host,
                    onValueChange = { vm.host = it },
                    label = { Text("Server address") },
                    supportingText = { Text("Only change this if your admin asks you to.") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
            // Not tied to one field: wrong password, no route, an untrusted certificate and "wait" all land here.
            vm.error?.let { ErrorNote(it) }
            Button(
                colors = brassButton(),
                onClick = vm::submit,
                enabled = vm.canSubmit,
                modifier = Modifier.fillMaxWidth().height(52.dp),
            ) {
                if (vm.busy) {
                    CircularProgressIndicator(
                        strokeWidth = 2.dp,
                        color = LocalContentColor.current, // the disabled button's content colour, so it shows
                        modifier = Modifier.size(20.dp),
                    )
                } else {
                    Text("Sign in")
                }
            }
            TextButton(onClick = { vm.advanced = !vm.advanced }, modifier = Modifier.offset(x = (-12).dp)) {
                Text(if (vm.advanced) "Hide server address" else "Server address")
            }
        }
      }
    }
}
